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
#include "cofinance/service/LedgerService.h"
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
    return "p6" + tag.substr(0, 1) + std::to_string(pid) + std::to_string(now) +
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
    const char* statements[] = {
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

cofinance::service::ParticipantInput participant(std::uint64_t user_id) {
    cofinance::service::ParticipantInput value;
    value.user_id = user_id;
    return value;
}

cofinance::service::ParticipantInput participant(std::uint64_t user_id,
                                                  std::int64_t share_cents) {
    cofinance::service::ParticipantInput value;
    value.user_id = user_id;
    value.share_cents = share_cents;
    return value;
}

cofinance::service::TransactionInput transactionInput(
    int type, std::int64_t amount_cents, std::uint64_t payer_id,
    std::vector<cofinance::service::ParticipantInput> participants, int split_type,
    std::uint64_t category_id = 0, std::string note = std::string(),
    std::optional<std::string> client_token = std::nullopt) {
    cofinance::service::TransactionInput input;
    input.type = type;
    input.amount_cents = amount_cents;
    input.category_id = category_id;
    input.note = std::move(note);
    input.payer_id = payer_id;
    input.participants = std::move(participants);
    input.split_type = split_type;
    input.client_token = std::move(client_token);
    return input;
}

void joinGroup(cofinance::service::GroupService& groups, std::uint64_t owner,
               std::uint64_t group_id, std::uint64_t user_id, const std::string& username) {
    cofinance::service::ServiceError error;
    cofinance::db::InviteRecord invite;
    CHECK(groups.inviteMember(owner, group_id, username, &invite, &error));
    std::uint64_t accepted_group = 0;
    CHECK(groups.acceptInvite(user_id, invite.id, &accepted_group, &error));
    CHECK(accepted_group == group_id);
}

void testLedger(cofinance::db::ConnectionPool& pool,
                cofinance::db::MySqlConnection& connection,
                const std::vector<std::uint64_t>& users,
                const std::vector<std::string>& names,
                std::vector<std::uint64_t>* groups_to_clean) {
    cofinance::service::GroupService groups(pool);
    cofinance::service::LedgerService ledger(pool);
    cofinance::service::ServiceError error;
    std::string db_error;
    std::string value;
    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    const std::uint64_t carol = users[2];
    const std::uint64_t dave = users[3];
    const std::string token_one = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    const std::string token_one_upper = "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA";
    const std::string token_two = "22222222-2222-4222-8222-222222222222";
    const std::string token_shared = "33333333-3333-4333-8333-333333333333";
    const std::string token_cross = "44444444-4444-4444-8444-444444444444";

    std::printf("[case] 分类基础、权限与重复名\n");
    cofinance::db::GroupRecord group;
    CHECK(groups.createGroup(alice, "账本测试组", &group, &error));
    groups_to_clean->push_back(group.id);
    joinGroup(groups, alice, group.id, bob, names[1]);
    joinGroup(groups, alice, group.id, carol, names[2]);

    cofinance::db::GroupRecord second_group;
    CHECK(groups.createGroup(dave, "第二组", &second_group, &error));
    groups_to_clean->push_back(second_group.id);

    std::vector<cofinance::db::CategoryRecord> categories;
    CHECK(ledger.listCategories(alice, std::nullopt, &categories, &error));
    CHECK(categories.size() >= 9);
    const std::uint64_t system_category = categories.front().id;
    CHECK(ledger.listCategories(alice, group.id, &categories, &error));
    CHECK(categories.size() >= 9);

    cofinance::db::CategoryRecord custom_category;
    CHECK(ledger.createCategory(bob, group.id, "  宠物  ", "paw", 5, &custom_category, &error));
    CHECK(custom_category.name == "宠物" && custom_category.group_id == group.id);
    CHECK(!ledger.createCategory(alice, group.id, "宠物", "", 6, &custom_category, &error));
    CHECK(error.code == 1016);
    CHECK(!ledger.createCategory(dave, group.id, "越权分类", "", 6, &custom_category, &error));
    CHECK(error.code == 1004);
    CHECK(ledger.listCategories(alice, group.id, &categories, &error));
    CHECK(categories.size() >= 10);
    CHECK(ledger.listCategories(bob, std::nullopt, &categories, &error));
    CHECK(categories.size() >= 9);
    cofinance::db::CategoryRecord second_category;
    CHECK(ledger.createCategory(dave, second_group.id, "宠物", "paw", 5, &second_category,
                                &error));

    std::printf("[case] 金额、分摊与关系校验\n");
    cofinance::db::TransactionDetail detail;
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(2, 100, alice, {participant(bob)}, 0), &detail, &error));
    CHECK(error.code == 1001);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 0, alice, {participant(bob)}, 0), &detail, &error));
    CHECK(error.code == 1001);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, alice, {participant(bob), participant(bob)}, 0), &detail,
        &error));
    CHECK(error.code == 1001);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, alice, {participant(bob, 40), participant(carol, 50)}, 1),
        &detail, &error));
    CHECK(error.code == 1010);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, alice, {participant(bob)}, 0, 0, "", std::string("bad")),
        &detail, &error));
    CHECK(error.code == 1001);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, dave, {participant(bob)}, 0), &detail, &error));
    CHECK(error.code == 1013);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, alice, {participant(dave)}, 0), &detail, &error));
    CHECK(error.code == 1013);
    CHECK(!ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 100, alice, {participant(bob)}, 0, second_category.id), &detail,
        &error));
    CHECK(error.code == 1001);
    CHECK(!ledger.createTransaction(
        dave, group.id,
        transactionInput(0, 100, alice, {participant(bob)}, 0), &detail, &error));
    CHECK(error.code == 1004);

    std::printf("[case] 均分余数、幂等与详情\n");
    cofinance::service::TransactionInput equal_input =
        transactionInput(0, 10000, alice, {participant(alice), participant(bob),
                                           participant(carol)},
                         0, custom_category.id, "晚餐", token_one);
    equal_input.client_token = token_one_upper;
    CHECK(ledger.createTransaction(alice, group.id, equal_input, &detail, &error));
    const std::uint64_t transaction_one = detail.transaction.id;
    CHECK(detail.transaction.amount_cents == 10000);
    CHECK(detail.transaction.category_name == "宠物");
    CHECK(detail.participants.size() == 3);
    CHECK(detail.participants[0].share_cents == 3334);
    CHECK(detail.participants[1].share_cents == 3333);
    CHECK(detail.participants[2].share_cents == 3333);
    std::int64_t sum = 0;
    for (const cofinance::db::ParticipantRecord& item : detail.participants) {
        sum += item.share_cents;
    }
    CHECK(sum == 10000);
    CHECK(ledger.createTransaction(alice, group.id, equal_input, &detail, &error));
    CHECK(detail.transaction.id == transaction_one);
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM transactions WHERE group_id = ? AND client_token = ?",
                      {cofinance::db::SqlParam{group.id},
                       cofinance::db::SqlParam{token_one}}, &value, &db_error));
    CHECK(value == "1");

    std::printf("[case] 幂等键大小写与跨组冲突\n");
    equal_input.client_token = token_one;
    CHECK(ledger.createTransaction(alice, group.id, equal_input, &detail, &error));
    CHECK(detail.transaction.id == transaction_one);

    cofinance::db::GroupRecord third_group;
    CHECK(groups.createGroup(alice, "第三组", &third_group, &error));
    groups_to_clean->push_back(third_group.id);
    cofinance::db::GroupRecord fourth_group;
    CHECK(groups.createGroup(alice, "第四组", &fourth_group, &error));
    groups_to_clean->push_back(fourth_group.id);
    cofinance::service::TransactionInput cross_input = transactionInput(
        0, 100, alice, {participant(alice)}, 0, 0, "跨组", token_cross);
    CHECK(ledger.createTransaction(alice, third_group.id, cross_input, &detail, &error));
    CHECK(!ledger.createTransaction(alice, fourth_group.id, cross_input, &detail, &error));
    CHECK(error.code == 1018 && error.http_status == 409);

    std::printf("[case] 自定义分摊、修改与权限\n");
    cofinance::service::TransactionInput custom_input = transactionInput(
        1, 9000, bob, {participant(alice, 6000), participant(bob, 3000)}, 1, 0, "奖金",
        token_two);
    CHECK(ledger.createTransaction(bob, group.id, custom_input, &detail, &error));
    const std::uint64_t transaction_two = detail.transaction.id;
    CHECK(detail.transaction.type == 1 && detail.participants[0].share_cents == 6000);
    CHECK(!ledger.updateTransaction(
        carol, group.id, transaction_one,
        transactionInput(0, 100, alice, {participant(alice)}, 0), &detail, &error));
    CHECK(error.code == 1004);
    CHECK(ledger.updateTransaction(
        alice, group.id, transaction_two,
        transactionInput(0, 5000, bob, {participant(bob), participant(carol)}, 0, 0, "改账",
                         std::nullopt),
        &detail, &error));
    CHECK(detail.transaction.amount_cents == 5000);
    CHECK(detail.participants.size() == 2);
    CHECK(detail.participants[0].share_cents == 2500 &&
          detail.participants[1].share_cents == 2500);

    std::printf("[case] 游标分页与筛选\n");
    cofinance::db::TransactionDetail tx3;
    cofinance::db::TransactionDetail tx4;
    cofinance::db::TransactionDetail tx5;
    CHECK(ledger.createTransaction(
        carol, group.id,
        transactionInput(0, 100, carol, {participant(carol)}, 0, 0, "tx3"), &tx3, &error));
    CHECK(ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 200, alice, {participant(alice)}, 0, custom_category.id, "tx4"),
        &tx4, &error));
    CHECK(ledger.createTransaction(
        bob, group.id,
        transactionInput(0, 300, bob, {participant(bob)}, 0, 0, "tx5"), &tx5, &error));

    cofinance::service::TransactionFilterInput filter;
    filter.limit = 2;
    cofinance::service::TransactionPage page;
    CHECK(ledger.listTransactions(alice, group.id, filter, &page, &error));
    CHECK(page.items.size() == 2 && page.next_cursor.has_value());
    const std::uint64_t first_cursor = *page.next_cursor;
    filter.cursor = first_cursor;
    CHECK(ledger.listTransactions(alice, group.id, filter, &page, &error));
    CHECK(page.items.size() == 2 && page.next_cursor.has_value());
    filter.cursor = page.next_cursor;
    CHECK(ledger.listTransactions(alice, group.id, filter, &page, &error));
    CHECK(page.items.size() == 1 && !page.next_cursor.has_value());

    cofinance::service::TransactionFilterInput category_filter;
    category_filter.category_id = custom_category.id;
    CHECK(ledger.listTransactions(alice, group.id, category_filter, &page, &error));
    CHECK(page.items.size() == 2);
    CHECK(ledger.listTransactions(dave, group.id, category_filter, &page, &error) == false);
    CHECK(error.code == 1004);
    cofinance::service::TransactionFilterInput payer_filter;
    payer_filter.payer_id = bob;
    CHECK(ledger.listTransactions(alice, group.id, payer_filter, &page, &error));
    CHECK(page.items.size() == 2);
    cofinance::service::TransactionFilterInput time_filter;
    time_filter.from = "2000-01-01";
    time_filter.to = "2099-12-31";
    CHECK(ledger.listTransactions(alice, group.id, time_filter, &page, &error));
    CHECK(page.items.size() == 5);
    time_filter.from = "2099-01-01";
    time_filter.to.reset();
    CHECK(ledger.listTransactions(alice, group.id, time_filter, &page, &error));
    CHECK(page.items.empty());
    time_filter.from = "bad-time";
    CHECK(!ledger.listTransactions(alice, group.id, time_filter, &page, &error));
    CHECK(error.code == 1001);

    std::printf("[case] 删除、软删过滤与分类保护\n");
    CHECK(!ledger.deleteTransaction(carol, group.id, transaction_one, &error));
    CHECK(error.code == 1004);
    CHECK(ledger.deleteTransaction(carol, group.id, tx3.transaction.id, &error));
    CHECK(!ledger.getTransaction(carol, group.id, tx3.transaction.id, &detail, &error));
    CHECK(error.code == 1005);
    CHECK(!ledger.deleteCategory(bob, group.id, custom_category.id, &error));
    CHECK(error.code == 1014);
    CHECK(!ledger.deleteCategory(alice, group.id, system_category, &error));
    CHECK(error.code == 1004);
    CHECK(ledger.deleteTransaction(alice, group.id, transaction_two, &error));
    CHECK(ledger.deleteTransaction(bob, group.id, transaction_one, &error) == false);
    CHECK(ledger.deleteTransaction(alice, group.id, transaction_one, &error));
    CHECK(ledger.deleteTransaction(alice, group.id, tx4.transaction.id, &error));
    CHECK(ledger.deleteCategory(bob, group.id, custom_category.id, &error));
    CHECK(!ledger.getTransaction(alice, group.id, transaction_one, &detail, &error));
    CHECK(error.code == 1005);

    std::printf("[case] 并发幂等键只落一笔\n");
    cofinance::service::TransactionInput concurrent_input =
        transactionInput(0, 5000, alice, {participant(alice), participant(bob)}, 0, 0,
                         "并发", token_shared);
    struct Attempt {
        bool ok = false;
        int code = 0;
        std::uint64_t id = 0;
    };
    Attempt first;
    Attempt second;
    std::thread worker_one([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::TransactionDetail local_detail;
        first.ok = ledger.createTransaction(alice, group.id, concurrent_input, &local_detail,
                                            &local_error);
        first.code = local_error.code;
        first.id = local_detail.transaction.id;
    });
    std::thread worker_two([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::TransactionDetail local_detail;
        second.ok = ledger.createTransaction(alice, group.id, concurrent_input, &local_detail,
                                             &local_error);
        second.code = local_error.code;
        second.id = local_detail.transaction.id;
    });
    worker_one.join();
    worker_two.join();
    CHECK(first.ok && second.ok);
    CHECK(first.id == second.id && first.id != 0);
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM transactions WHERE created_by = ? AND client_token = ?",
                      {cofinance::db::SqlParam{alice}, cofinance::db::SqlParam{token_shared}},
                      &value, &db_error));
    CHECK(value == "1");

    CHECK(ledger.deleteCategory(dave, second_group.id, second_category.id, &error));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";
    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err) || !cfg.loadFile(siblingPath(config_path, "secrets.conf"),
                                                          &err)) {
        std::fprintf(stderr, "P6 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    cofinance::db::ConnectionPool pool;
    if (!pool.init(makeConfig(cfg), &err)) {
        std::fprintf(stderr, "P6 测试连接池初始化失败: %s\n", err.c_str());
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
        for (int i = 0; i < 4; ++i) {
            const std::string name = uniqueName("u", i);
            const std::uint64_t id = insertUser(connection, name, &db_error);
            if (id == 0) {
                std::fprintf(stderr, "创建 P6 测试用户失败: %s\n", db_error.c_str());
                return EXIT_FAILURE;
            }
            users.push_back(id);
            names.push_back(name);
        }
        testLedger(pool, connection, users, names, &groups);
        for (std::uint64_t group_id : groups) {
            hardDeleteGroup(connection, group_id);
        }
        for (std::uint64_t user_id : users) {
            hardDeleteUser(connection, user_id);
        }
    }
    if (g_failed != 0) {
        std::fprintf(stderr, "P6 账本测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P6 账本测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
