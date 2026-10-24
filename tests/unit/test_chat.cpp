#include <unistd.h>

#include <atomic>
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
#include "cofinance/db/MessageDao.h"
#include "cofinance/db/PreparedStatement.h"
#include "cofinance/service/ChatService.h"
#include "cofinance/service/GroupService.h"
#include "cofinance/service/LongPollRegistry.h"
#include "cofinance/util/Common.h"
#include "cofinance/util/Config.h"

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
        if (statement.find_first_not_of(" \t\r\n") != std::string::npos &&
            !connection.execute(statement, err)) {
            return false;
        }
        statement.clear();
    }
    return true;
}

std::string uniqueName(const std::string& tag, int suffix) {
    const long long pid = static_cast<long long>(::getpid()) % 100000;
    const long long now = cofinance::util::nowUtcMillis() % 1000000;
    return "p8" + tag + std::to_string(pid) + std::to_string(now) + std::to_string(suffix);
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

std::uint64_t insertGroup(cofinance::db::MySqlConnection& connection,
                          const std::string& name, std::uint64_t owner_id, std::string* err) {
    cofinance::db::PreparedStatement statement(
        connection, "INSERT INTO finance_groups (name, owner_id, status) VALUES (?, ?, 0)",
        err);
    if (!statement.valid() ||
        !statement.bind({cofinance::db::SqlParam{name}, cofinance::db::SqlParam{owner_id}},
                        err) ||
        !statement.execute(err)) {
        return 0;
    }
    return statement.lastInsertId();
}

bool insertMember(cofinance::db::MySqlConnection& connection, std::uint64_t group_id,
                  std::uint64_t user_id, int role, std::string* err) {
    cofinance::db::PreparedStatement statement(
        connection,
        "INSERT INTO group_members (group_id, user_id, role, status, joined_at) "
        "VALUES (?, ?, ?, 1, UTC_TIMESTAMP())",
        err);
    return statement.valid() &&
           statement.bind({cofinance::db::SqlParam{group_id}, cofinance::db::SqlParam{user_id},
                           cofinance::db::SqlParam{static_cast<std::int64_t>(role)}}, err) &&
           statement.execute(err);
}

bool insertSystemMessage(cofinance::db::MySqlConnection& connection, std::uint64_t group_id,
                         const std::string& content, std::string* err) {
    cofinance::db::PreparedStatement statement(
        connection,
        "INSERT INTO messages (group_id, sender_id, type, content) VALUES (?, 0, 1, ?)", err);
    return statement.valid() &&
           statement.bind({cofinance::db::SqlParam{group_id}, cofinance::db::SqlParam{content}},
                          err) &&
           statement.execute(err);
}

void hardDeleteGroup(cofinance::db::MySqlConnection& connection, std::uint64_t group_id) {
    std::string err;
    const char* statements[] = {
        "DELETE FROM settlements WHERE group_id = ?",
        "DELETE FROM messages WHERE group_id = ?",
        "DELETE p FROM transaction_participants p JOIN transactions t "
        "ON t.id = p.transaction_id WHERE t.group_id = ?",
        "DELETE FROM transactions WHERE group_id = ?",
        "DELETE FROM categories WHERE group_id = ?",
        "DELETE FROM group_members WHERE group_id = ?",
        "DELETE FROM finance_groups WHERE id = ?"};
    for (const char* sql : statements) {
        cofinance::db::PreparedStatement statement(connection, sql, &err);
        if (!statement.valid() || !statement.bind({cofinance::db::SqlParam{group_id}}, &err)) {
            continue;
        }
        statement.execute(&err);
    }
}

void hardDeleteUser(cofinance::db::MySqlConnection& connection, std::uint64_t user_id) {
    std::string err;
    const char* statements[] = {"DELETE FROM sessions WHERE user_id = ?",
                                "DELETE FROM group_members WHERE user_id = ?",
                                "DELETE FROM users WHERE id = ?"};
    for (const char* sql : statements) {
        cofinance::db::PreparedStatement statement(connection, sql, &err);
        if (!statement.valid() || !statement.bind({cofinance::db::SqlParam{user_id}}, &err)) {
            continue;
        }
        statement.execute(&err);
    }
}

void testRegistryLongPoll() {
    std::printf("[case] 长轮询登记、按组唤醒与配额\n");
    cofinance::service::LongPollRegistry registry({2, 3, 10});
    std::atomic<int> wakes{0};
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t third = 0;
    auto wake = [&wakes]() { wakes.fetch_add(1); };
    CHECK(registry.add(10, 1, 0, wake, &first) ==
          cofinance::service::LongPollRegisterResult::Ok);
    CHECK(registry.add(10, 1, 0, wake, &second) ==
          cofinance::service::LongPollRegisterResult::Ok);
    CHECK(registry.add(10, 1, 0, wake, &third) ==
          cofinance::service::LongPollRegisterResult::UserLimit);
    CHECK(registry.countForUser(1) == 2);
    CHECK(registry.countForGroup(10) == 2);
    registry.notify(10);
    CHECK(wakes.load() == 2);
    registry.notify(20);
    CHECK(wakes.load() == 2);
    registry.remove(first);
    CHECK(registry.size() == 1);
    registry.remove(second);
    CHECK(registry.size() == 0);
}

void testChatService(cofinance::db::ConnectionPool& pool,
                     cofinance::db::MySqlConnection& connection,
                     const std::vector<std::uint64_t>& users,
                     const std::vector<std::string>& names,
                     std::vector<std::uint64_t>* groups_to_clean) {
    std::printf("[case] 消息发送、幂等、分页与已读水位线\n");
    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    const std::uint64_t carol = users[2];
    std::string db_error;
    const std::uint64_t group_id = insertGroup(connection, "P8 聊天组", alice, &db_error);
    CHECK(group_id != 0);
    const std::uint64_t other_group = insertGroup(connection, "P8 其他组", alice, &db_error);
    CHECK(other_group != 0);
    groups_to_clean->push_back(group_id);
    groups_to_clean->push_back(other_group);
    CHECK(insertMember(connection, group_id, alice, 0, &db_error));
    CHECK(insertMember(connection, group_id, bob, 1, &db_error));
    CHECK(insertMember(connection, other_group, alice, 0, &db_error));

    cofinance::service::ChatService chat(pool);
    cofinance::service::ServiceError error;
    cofinance::db::MessageRecord message;

    CHECK(!chat.sendMessage(alice, group_id, "   ", std::nullopt, &message, &error));
    CHECK(error.code == 1001);
    CHECK(!chat.sendMessage(alice, group_id, "bad", std::string("not-a-uuid"), &message,
                            &error));
    CHECK(error.code == 1001);
    CHECK(!chat.sendMessage(carol, group_id, "越权", std::nullopt, &message, &error));
    CHECK(error.code == 1004);

    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    const std::string upper_uuid = "11111111-2222-3333-4444-555555555555";
    CHECK(chat.sendMessage(alice, group_id, "第一条", upper_uuid, &message, &error));
    const std::uint64_t first_id = message.id;
    CHECK(first_id != 0);
    CHECK(message.sender_username == names[0]);
    CHECK(chat.sendMessage(alice, group_id, "重放不应新增", uuid, &message, &error));
    CHECK(message.id == first_id);
    CHECK(message.content == "第一条");

    cofinance::db::MessageRecord concurrent_one;
    cofinance::db::MessageRecord concurrent_two;
    cofinance::service::ServiceError error_one;
    cofinance::service::ServiceError error_two;
    const std::string concurrent_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    bool ok_one = false;
    bool ok_two = false;
    std::thread worker_one([&]() {
        ok_one = chat.sendMessage(alice, group_id, "并发幂等", concurrent_uuid, &concurrent_one,
                                  &error_one);
    });
    std::thread worker_two([&]() {
        ok_two = chat.sendMessage(alice, group_id, "并发幂等", concurrent_uuid, &concurrent_two,
                                  &error_two);
    });
    worker_one.join();
    worker_two.join();
    CHECK(ok_one && ok_two);
    CHECK(concurrent_one.id == concurrent_two.id);

    CHECK(!chat.sendMessage(alice, other_group, "跨组复用", uuid, &message, &error));
    CHECK(error.code == 1018);

    CHECK(chat.sendMessage(bob, group_id, "第二条", std::nullopt, &message, &error));
    const std::uint64_t second_id = message.id;
    CHECK(second_id > concurrent_one.id);
    CHECK(insertSystemMessage(connection, group_id, "组余额快照", &db_error));

    cofinance::service::MessageListInput list;
    list.limit = 1;
    cofinance::service::MessagePage page;
    CHECK(chat.listMessages(bob, group_id, list, &page, &error));
    CHECK(page.items.size() == 1);
    CHECK(page.items[0].type == 1);
    CHECK(page.items[0].sender_id == 0);
    CHECK(page.items[0].content == "组余额快照");
    CHECK(page.next_cursor.has_value());
    const std::uint64_t system_id = page.items[0].id;
    CHECK(page.server_max_id == system_id);

    list.cursor = page.next_cursor;
    CHECK(chat.listMessages(bob, group_id, list, &page, &error));
    CHECK(page.items.size() == 1);
    CHECK(page.items[0].id == second_id);

    list.cursor.reset();
    list.after_id = first_id;
    list.limit = 20;
    CHECK(chat.listMessages(bob, group_id, list, &page, &error));
    CHECK(!page.items.empty());
    CHECK(page.items.front().id > first_id);
    for (std::size_t i = 1; i < page.items.size(); ++i) {
        CHECK(page.items[i - 1].id < page.items[i].id);
    }
    list.cursor = first_id;
    CHECK(!chat.listMessages(bob, group_id, list, &page, &error));
    CHECK(error.code == 1001);

    cofinance::service::UnreadState unread;
    CHECK(chat.getUnread(bob, group_id, &unread, &error));
    CHECK(unread.last_read_message_id == 0);
    CHECK(unread.max_message_id == system_id);
    CHECK(unread.unread_count == 4);
    CHECK(chat.markRead(bob, group_id, second_id, &unread, &error));
    CHECK(unread.last_read_message_id == second_id);
    CHECK(unread.unread_count == 1);
    CHECK(chat.markRead(bob, group_id, first_id, &unread, &error));
    CHECK(unread.last_read_message_id == second_id);
    CHECK(chat.markRead(bob, group_id, 9999999, &unread, &error));
    CHECK(unread.last_read_message_id == system_id);
    CHECK(unread.unread_count == 0);

    cofinance::service::PollSnapshot snapshot;
    CHECK(chat.pollSnapshot(bob, group_id, std::nullopt, 100, &snapshot, &error));
    CHECK(snapshot.items.empty());
    CHECK(snapshot.effective_after_id == system_id);
    CHECK(snapshot.server_max_id == system_id);
    CHECK(chat.pollSnapshot(bob, group_id, first_id, 100, &snapshot, &error));
    CHECK(!snapshot.items.empty());
    CHECK(snapshot.items.front().id > first_id);
    CHECK(chat.pollSnapshot(bob, group_id, system_id, 100, &snapshot, &error));
    CHECK(snapshot.items.empty());

    cofinance::service::GroupService groups(pool);
    std::vector<cofinance::db::GroupSummaryRecord> summaries;
    CHECK(groups.listGroups(alice, &summaries, &error));
    const cofinance::db::GroupSummaryRecord* summary = nullptr;
    for (const cofinance::db::GroupSummaryRecord& candidate : summaries) {
        if (candidate.group.id == group_id) {
            summary = &candidate;
            break;
        }
    }
    CHECK(summary != nullptr);
    if (summary != nullptr) {
        CHECK(summary->last_message_id == system_id);
        CHECK(summary->last_message_type == 1);
        CHECK(summary->last_message_sender_name == "系统");
        CHECK(summary->unread_count == 4);
    }

    std::uint64_t waiter_id = 0;
    CHECK(chat.registerWaiter(bob, group_id, 0, []() {}, &waiter_id, &error));
    CHECK(chat.pendingPolls() == 1);
    chat.removeWaiter(waiter_id);
    CHECK(chat.pendingPolls() == 0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";
    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err) ||
        !cfg.loadFile(siblingPath(config_path, "secrets.conf"), &err)) {
        std::fprintf(stderr, "P8 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    cofinance::db::ConnectionPool pool;
    if (!pool.init(makeConfig(cfg), &err)) {
        std::fprintf(stderr, "P8 测试连接池初始化失败: %s\n", err.c_str());
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
        for (int i = 0; i < 3; ++i) {
            const std::string name = uniqueName("u", i);
            const std::uint64_t id = insertUser(connection, name, &db_error);
            if (id == 0) {
                std::fprintf(stderr, "创建 P8 测试用户失败: %s\n", db_error.c_str());
                return EXIT_FAILURE;
            }
            users.push_back(id);
            names.push_back(name);
        }
        testRegistryLongPoll();
        testChatService(pool, connection, users, names, &groups);
        for (std::uint64_t group_id : groups) {
            hardDeleteGroup(connection, group_id);
        }
        for (std::uint64_t user_id : users) {
            hardDeleteUser(connection, user_id);
        }
    }
    if (g_failed != 0) {
        std::fprintf(stderr, "P8 聊天测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P8 聊天测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
