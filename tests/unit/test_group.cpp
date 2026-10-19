#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/PreparedStatement.h"
#include "cofinance/service/GroupService.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Config.h"
#include "cofinance/util/Common.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                            \
    } while (false)

std::string siblingPath(const std::string& path, const std::string& name) {
    return (std::filesystem::path(path).parent_path() / name).string();
}

bool loadSchema(const std::string& path, cofinance::db::MySqlConnection& connection,
                std::string* err) {
    std::ifstream input(path);
    if (!input) {
        *err = "无法读取 " + path;
        return false;
    }
    std::string cleaned;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line.compare(first, 2, "--") == 0) {
            continue;
        }
        cleaned += line;
        cleaned.push_back('\n');
    }

    std::string statement;
    for (char c : cleaned) {
        if (c != ';') {
            statement.push_back(c);
            continue;
        }
        if (statement.find_first_not_of(" \t\r\n") != std::string::npos) {
            if (!connection.execute(statement, err)) {
                return false;
            }
        }
        statement.clear();
    }
    return true;
}

bool queryScalar(cofinance::db::MySqlConnection& connection, const std::string& sql,
                 const std::vector<cofinance::db::SqlParam>& params, std::string* value,
                 std::string* err) {
    cofinance::db::PreparedStatement statement(connection, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    cofinance::db::SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err) || !has_row || row.size() != 1 ||
        !row[0].has_value()) {
        if (err->empty()) {
            *err = "标量查询没有返回一行一列";
        }
        return false;
    }
    *value = *row[0];
    return true;
}

std::string uniqueName(const std::string& tag, int suffix) {
    const long long pid = static_cast<long long>(::getpid()) % 100000;
    const long long now = cofinance::util::nowUtcMillis() % 1000000;
    return "p5" + tag.substr(0, 1) + std::to_string(pid) + std::to_string(now) +
           std::to_string(suffix);
}

cofinance::db::MySqlConfig makeConfig(const cofinance::Config& cfg) {
    cofinance::db::MySqlConfig out;
    out.host = cfg.get("db.host", "127.0.0.1");
    out.port = static_cast<unsigned int>(cfg.getInt("db.port", 3306));
    out.user = cfg.get("db.user");
    out.password = cfg.get("db.password");
    out.database = cfg.get("db.name", "cofinance");
    out.pool_size = 8;
    out.connect_timeout_ms = static_cast<int>(cfg.getInt("db.connect_timeout_ms", 3000));
    out.acquire_timeout_ms = static_cast<int>(cfg.getInt("db.acquire_timeout_ms", 3000));
    return out;
}

std::uint64_t insertUser(cofinance::db::MySqlConnection& connection,
                         const std::string& username, std::string* err) {
    cofinance::db::PreparedStatement statement(
        connection,
        "INSERT INTO users (username, password_hash, nickname, status) VALUES (?, 'x', ?, 0)",
        err);
    if (!statement.valid() || !statement.bind({cofinance::db::SqlParam{username},
                                               cofinance::db::SqlParam{username}}, err) ||
        !statement.execute(err)) {
        return 0;
    }
    return statement.lastInsertId();
}

void hardDeleteGroup(cofinance::db::MySqlConnection& connection, std::uint64_t group_id) {
    std::string err;
    cofinance::db::PreparedStatement members(connection,
                                             "DELETE FROM group_members WHERE group_id = ?", &err);
    if (members.valid()) {
        members.bind({cofinance::db::SqlParam{group_id}}, &err);
        members.execute(&err);
    }
    cofinance::db::PreparedStatement groups(connection,
                                            "DELETE FROM finance_groups WHERE id = ?", &err);
    if (groups.valid()) {
        groups.bind({cofinance::db::SqlParam{group_id}}, &err);
        groups.execute(&err);
    }
}

void hardDeleteUser(cofinance::db::MySqlConnection& connection, std::uint64_t user_id) {
    std::string err;
    cofinance::db::PreparedStatement sessions(connection,
                                               "DELETE FROM sessions WHERE user_id = ?", &err);
    if (sessions.valid()) {
        sessions.bind({cofinance::db::SqlParam{user_id}}, &err);
        sessions.execute(&err);
    }
    cofinance::db::PreparedStatement members(connection,
                                             "DELETE FROM group_members WHERE user_id = ?", &err);
    if (members.valid()) {
        members.bind({cofinance::db::SqlParam{user_id}}, &err);
        members.execute(&err);
    }
    cofinance::db::PreparedStatement users(connection, "DELETE FROM users WHERE id = ?", &err);
    if (users.valid()) {
        users.bind({cofinance::db::SqlParam{user_id}}, &err);
        users.execute(&err);
    }
}

cofinance::service::AuthContext contextFor(std::uint64_t user_id) {
    cofinance::service::AuthContext context;
    context.user.id = user_id;
    context.token_hash = "p5-test-token-hash";
    return context;
}

void testGroupLifecycle(cofinance::db::ConnectionPool& pool,
                        cofinance::db::MySqlConnection& connection,
                        const std::vector<std::uint64_t>& users,
                        const std::vector<std::string>& names,
                        std::vector<std::uint64_t>* groups_to_clean,
                        std::vector<std::uint64_t>* users_to_clean) {
    cofinance::service::GroupService groups(pool);
    cofinance::service::UserService users_service(pool);
    cofinance::service::ServiceError error;
    std::string db_error;

    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    const std::uint64_t carol = users[2];
    const std::uint64_t dave = users[3];
    const std::uint64_t erin = users[4];
    const std::uint64_t outsider = users[5];

    std::printf("[case] 创建组、列表、详情与越权\n");
    cofinance::db::GroupRecord group;
    CHECK(!groups.createGroup(alice, "   ", &group, &error));
    CHECK(error.code == 1001);
    CHECK(!groups.createGroup(alice, std::string("\xff", 1), &group, &error));
    CHECK(error.code == 1001);
    CHECK(groups.createGroup(alice, "  宿舍402  ", &group, &error));
    CHECK(group.id != 0 && group.name == "宿舍402" && group.owner_id == alice);
    groups_to_clean->push_back(group.id);

    std::string value;
    CHECK(queryScalar(connection,
                      "SELECT CONCAT(role, ',', status) FROM group_members "
                      "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{alice}},
                      &value, &db_error));
    CHECK(value == "0,1");

    std::vector<cofinance::db::GroupSummaryRecord> summaries;
    CHECK(groups.listGroups(alice, &summaries, &error));
    CHECK(summaries.size() == 1 && summaries[0].group.id == group.id);
    CHECK(summaries[0].role == 0 && summaries[0].member_count == 1);
    int role = 1;
    std::uint64_t member_count = 0;
    CHECK(groups.getGroup(alice, group.id, &group, &role, &member_count, &error));
    CHECK(role == 0 && member_count == 1);
    CHECK(!groups.getGroup(outsider, group.id, &group, &role, &member_count, &error));
    CHECK(error.code == 1004 && error.http_status == 403);
    CHECK(!groups.getGroup(alice, 999999999ULL, &group, &role, &member_count, &error));
    CHECK(error.code == 1005);

    std::printf("[case] 邀请、接受、重复邀请与过期\n");
    cofinance::db::InviteRecord invite;
    CHECK(!groups.inviteMember(alice, group.id, names[0], &invite, &error));
    CHECK(error.code == 1011);
    CHECK(!groups.inviteMember(alice, group.id, "missing-user", &invite, &error));
    CHECK(error.code == 1005);
    CHECK(groups.inviteMember(alice, group.id, names[1], &invite, &error));
    CHECK(invite.user_id == bob && invite.status == 0);
    const std::uint64_t bob_invite = invite.id;
    CHECK(!groups.inviteMember(alice, group.id, names[1], &invite, &error));
    CHECK(error.code == 1008);

    std::vector<cofinance::db::InviteRecord> invites;
    CHECK(groups.listInvites(bob, &invites, &error));
    CHECK(invites.size() == 1 && invites[0].id == bob_invite);
    std::uint64_t accepted_group = 0;
    CHECK(!groups.acceptInvite(carol, bob_invite, &accepted_group, &error));
    CHECK(error.code == 1009);
    CHECK(groups.acceptInvite(bob, bob_invite, &accepted_group, &error));
    CHECK(accepted_group == group.id);
    CHECK(queryScalar(connection,
                      "SELECT CONCAT(role, ',', status) FROM group_members "
                      "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{bob}},
                      &value, &db_error));
    CHECK(value == "1,1");
    CHECK(!groups.acceptInvite(bob, bob_invite, &accepted_group, &error));
    CHECK(error.code == 1009);

    CHECK(groups.listInvites(erin, &invites, &error));
    CHECK(invites.empty());
    CHECK(groups.inviteMember(alice, group.id, names[4], &invite, &error));
    const std::uint64_t erin_invite = invite.id;
    cofinance::db::PreparedStatement expire(
        connection, "UPDATE group_members SET created_at = DATE_SUB(UTC_TIMESTAMP(), "
                    "INTERVAL 8 DAY) WHERE id = ?",
        &db_error);
    CHECK(expire.valid());
    CHECK(expire.bind({cofinance::db::SqlParam{erin_invite}}, &db_error));
    CHECK(expire.execute(&db_error));
    CHECK(groups.listInvites(erin, &invites, &error));
    CHECK(invites.empty());
    CHECK(queryScalar(connection, "SELECT status FROM group_members WHERE id = ?",
                      {cofinance::db::SqlParam{erin_invite}}, &value, &db_error));
    CHECK(value == "5");
    CHECK(!groups.acceptInvite(erin, erin_invite, &accepted_group, &error));
    CHECK(error.code == 1009);

    std::printf("[case] 角色权限、退出、移除与重新邀请\n");
    CHECK(!groups.renameGroup(bob, group.id, "新名字", &group, &error));
    CHECK(error.code == 1004);
    CHECK(groups.renameGroup(alice, group.id, "新名字", &group, &error));
    CHECK(group.name == "新名字");
    CHECK(!groups.leaveGroup(alice, group.id, &error));
    CHECK(error.code == 1017);
    CHECK(!groups.removeMember(alice, group.id, alice, &error));
    CHECK(error.code == 1017);
    CHECK(groups.leaveGroup(bob, group.id, &error));
    CHECK(queryScalar(connection, "SELECT status FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{bob}}, &value,
                      &db_error));
    CHECK(value == "3");
    CHECK(!groups.getGroup(bob, group.id, &group, &role, &member_count, &error));
    CHECK(error.code == 1004);

    CHECK(groups.inviteMember(alice, group.id, names[1], &invite, &error));
    const std::uint64_t bob_reinvite = invite.id;
    CHECK(groups.acceptInvite(bob, bob_reinvite, &accepted_group, &error));
    CHECK(queryScalar(connection, "SELECT COUNT(*) FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{bob}}, &value,
                      &db_error));
    CHECK(value == "1");

    CHECK(!groups.removeMember(bob, group.id, alice, &error));
    CHECK(error.code == 1004);
    CHECK(groups.removeMember(alice, group.id, bob, &error));
    CHECK(queryScalar(connection, "SELECT status FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{bob}}, &value,
                      &db_error));
    CHECK(value == "4");
    CHECK(!groups.getGroup(bob, group.id, &group, &role, &member_count, &error));
    CHECK(error.code == 1004);

    std::printf("[case] 拒绝邀请与组长转让\n");
    CHECK(groups.inviteMember(alice, group.id, names[3], &invite, &error));
    const std::uint64_t dave_invite = invite.id;
    CHECK(groups.rejectInvite(dave, dave_invite, &error));
    CHECK(queryScalar(connection, "SELECT status FROM group_members WHERE id = ?",
                      {cofinance::db::SqlParam{dave_invite}}, &value, &db_error));
    CHECK(value == "2");
    CHECK(!groups.acceptInvite(dave, dave_invite, &accepted_group, &error));
    CHECK(error.code == 1009);

    CHECK(!groups.transferOwner(bob, group.id, carol, &error));
    CHECK(error.code == 1004);
    CHECK(!groups.transferOwner(alice, group.id, outsider, &error));
    CHECK(error.code == 1017);
    CHECK(groups.inviteMember(alice, group.id, names[2], &invite, &error));
    CHECK(groups.acceptInvite(carol, invite.id, &accepted_group, &error));
    CHECK(groups.transferOwner(alice, group.id, carol, &error));
    CHECK(queryScalar(connection, "SELECT owner_id FROM finance_groups WHERE id = ?",
                      {cofinance::db::SqlParam{group.id}}, &value, &db_error));
    CHECK(value == std::to_string(carol));
    CHECK(queryScalar(connection, "SELECT CONCAT(role, ',', status) FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{alice}}, &value,
                      &db_error));
    CHECK(value == "1,1");
    CHECK(queryScalar(connection, "SELECT CONCAT(role, ',', status) FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{group.id}, cofinance::db::SqlParam{carol}}, &value,
                      &db_error));
    CHECK(value == "0,1");
    CHECK(!groups.dissolveGroup(alice, group.id, &error));
    CHECK(error.code == 1004);
    CHECK(groups.dissolveGroup(carol, group.id, &error));
    CHECK(!groups.getGroup(carol, group.id, &group, &role, &member_count, &error));
    CHECK(error.code == 1005);
    CHECK(groups.listGroups(carol, &summaries, &error));
    CHECK(summaries.empty());

    std::printf("[case] 组容量与并发邀请不突破上限\n");
    cofinance::db::GroupRecord capacity_group;
    CHECK(groups.createGroup(alice, "容量测试组", &capacity_group, &error));
    groups_to_clean->push_back(capacity_group.id);
    std::vector<std::pair<std::uint64_t, std::string>> capacity_users;
    for (int i = 0; i < 20; ++i) {
        const std::string name = uniqueName("c", i);
        const std::uint64_t id = insertUser(connection, name, &db_error);
        CHECK(id != 0);
        capacity_users.emplace_back(id, name);
        users_to_clean->push_back(id);
    }
    for (int i = 0; i < 18; ++i) {
        CHECK(groups.inviteMember(alice, capacity_group.id, capacity_users[static_cast<std::size_t>(i)].second,
                                  &invite, &error));
    }
    CHECK(queryScalar(connection, "SELECT COUNT(*) FROM group_members "
                                  "WHERE group_id = ? AND status IN (0, 1)",
                      {cofinance::db::SqlParam{capacity_group.id}}, &value, &db_error));
    CHECK(value == "19");

    struct InviteAttempt {
        bool ok = false;
        int code = 0;
    };
    InviteAttempt attempt1;
    InviteAttempt attempt2;
    std::thread first([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::InviteRecord local_invite;
        attempt1.ok = groups.inviteMember(alice, capacity_group.id, capacity_users[18].second,
                                          &local_invite, &local_error);
        attempt1.code = local_error.code;
    });
    std::thread second([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::InviteRecord local_invite;
        attempt2.ok = groups.inviteMember(alice, capacity_group.id, capacity_users[19].second,
                                          &local_invite, &local_error);
        attempt2.code = local_error.code;
    });
    first.join();
    second.join();
    CHECK(attempt1.ok != attempt2.ok);
    CHECK((attempt1.ok ? attempt2.code : attempt1.code) == 1012);
    CHECK(queryScalar(connection, "SELECT COUNT(*) FROM group_members "
                                  "WHERE group_id = ? AND status IN (0, 1)",
                      {cofinance::db::SqlParam{capacity_group.id}}, &value, &db_error));
    CHECK(value == "20");

    std::printf("[case] 注销账号与组成员关系\n");
    cofinance::db::GroupRecord owner_group;
    CHECK(groups.createGroup(outsider, "注销保护组", &owner_group, &error));
    groups_to_clean->push_back(owner_group.id);
    CHECK(!users_service.deleteAccount(contextFor(outsider), &error));
    CHECK(error.code == 1017);
    CHECK(queryScalar(connection, "SELECT COUNT(*) FROM users WHERE id = ? AND status = 0",
                      {cofinance::db::SqlParam{outsider}}, &value, &db_error));
    CHECK(value == "1");

    CHECK(groups.inviteMember(outsider, owner_group.id, names[0], &invite, &error));
    CHECK(groups.acceptInvite(alice, invite.id, &accepted_group, &error));
    CHECK(groups.transferOwner(outsider, owner_group.id, alice, &error));
    CHECK(users_service.deleteAccount(contextFor(outsider), &error));
    CHECK(queryScalar(connection, "SELECT status FROM users WHERE id = ?",
                      {cofinance::db::SqlParam{outsider}}, &value, &db_error));
    CHECK(value == "2");
    CHECK(queryScalar(connection, "SELECT status FROM group_members "
                                  "WHERE group_id = ? AND user_id = ?",
                      {cofinance::db::SqlParam{owner_group.id},
                       cofinance::db::SqlParam{outsider}},
                      &value, &db_error));
    CHECK(value == "3");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";

    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err) || !cfg.loadFile(siblingPath(config_path, "secrets.conf"),
                                                          &err)) {
        std::fprintf(stderr, "P5 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    cofinance::db::ConnectionPool pool;
    if (!pool.init(makeConfig(cfg), &err)) {
        std::fprintf(stderr, "P5 测试连接池初始化失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    std::vector<std::uint64_t> users;
    std::vector<std::string> names;
    std::vector<std::uint64_t> groups;
    {
        std::string db_error;
        auto lease = pool.acquire(-1, &db_error);
        if (!lease) {
            std::fprintf(stderr, "无法获取测试连接: %s\n", db_error.c_str());
            return EXIT_FAILURE;
        }
        cofinance::db::MySqlConnection& connection = lease.connection();
        if (!loadSchema(schema_path, connection, &db_error)) {
            std::fprintf(stderr, "加载 schema 失败: %s\n", db_error.c_str());
            return EXIT_FAILURE;
        }
        for (int i = 0; i < 6; ++i) {
            const std::string name = uniqueName("u", i);
            const std::uint64_t id = insertUser(connection, name, &db_error);
            if (id == 0) {
                std::fprintf(stderr, "创建 P5 测试用户失败: %s\n", db_error.c_str());
                return EXIT_FAILURE;
            }
            users.push_back(id);
            names.push_back(name);
        }
        testGroupLifecycle(pool, connection, users, names, &groups, &users);
        for (std::uint64_t group_id : groups) {
            hardDeleteGroup(connection, group_id);
        }
        for (std::uint64_t user_id : users) {
            hardDeleteUser(connection, user_id);
        }
    }

    if (g_failed != 0) {
        std::fprintf(stderr, "P5 组与成员测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P5 组与成员测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
