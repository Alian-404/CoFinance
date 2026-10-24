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
#include "cofinance/service/SettlementService.h"
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
    return "p7" + tag.substr(0, 1) + std::to_string(pid) + std::to_string(now) +
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
    std::vector<cofinance::service::ParticipantInput> participants,
    std::uint64_t category_id = 0, int split_type = 0) {
    cofinance::service::TransactionInput input;
    input.type = type;
    input.amount_cents = amount_cents;
    input.category_id = category_id;
    input.note = "P7";
    input.payer_id = payer_id;
    input.participants = std::move(participants);
    input.split_type = split_type;
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

const cofinance::db::BalanceRecord* findBalance(
    const std::vector<cofinance::db::BalanceRecord>& balances, std::uint64_t user_id) {
    for (const cofinance::db::BalanceRecord& balance : balances) {
        if (balance.user_id == user_id) {
            return &balance;
        }
    }
    return nullptr;
}

const cofinance::db::CategoryStatRecord* findCategory(
    const std::vector<cofinance::db::CategoryStatRecord>& categories, int type,
    std::uint64_t category_id) {
    for (const cofinance::db::CategoryStatRecord& category : categories) {
        if (category.type == type && category.category_id == category_id) {
            return &category;
        }
    }
    return nullptr;
}

const cofinance::db::MemberStatRecord* findMember(
    const std::vector<cofinance::db::MemberStatRecord>& members, std::uint64_t user_id) {
    for (const cofinance::db::MemberStatRecord& member : members) {
        if (member.user_id == user_id) {
            return &member;
        }
    }
    return nullptr;
}

void testBalancesStatsAndSettlements(cofinance::db::ConnectionPool& pool,
                                     cofinance::db::MySqlConnection& connection,
                                     const std::vector<std::uint64_t>& users,
                                     const std::vector<std::string>& names,
                                     std::vector<std::uint64_t>* groups_to_clean) {
    cofinance::service::GroupService groups(pool);
    cofinance::service::LedgerService ledger(pool);
    cofinance::service::SettlementService settlements(pool);
    cofinance::service::ServiceError error;
    std::string db_error;
    std::string value;
    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    const std::uint64_t carol = users[2];
    const std::uint64_t dave = users[3];

    std::printf("[case] 支出、收入、统计与净额\n");
    cofinance::db::GroupRecord group;
    CHECK(groups.createGroup(alice, "P7 主账本", &group, &error));
    groups_to_clean->push_back(group.id);
    joinGroup(groups, alice, group.id, bob, names[1]);
    joinGroup(groups, alice, group.id, carol, names[2]);
    std::vector<cofinance::db::CategoryRecord> categories;
    CHECK(ledger.listCategories(alice, std::nullopt, &categories, &error));
    CHECK(categories.size() >= 2);
    const std::uint64_t category_id = categories.front().id;

    cofinance::db::TransactionDetail detail;
    CHECK(ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 9000, alice, {participant(alice), participant(bob),
                                          participant(carol)},
                         category_id),
        &detail, &error));
    CHECK(ledger.createTransaction(
        bob, group.id,
        transactionInput(1, 3000, bob, {participant(alice), participant(bob)}, category_id),
        &detail, &error));

    cofinance::service::BalanceSummary summary;
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    CHECK(summary.items.size() == 3);
    CHECK(summary.total_balance_cents == 0);
    const cofinance::db::BalanceRecord* alice_balance = findBalance(summary.items, alice);
    const cofinance::db::BalanceRecord* bob_balance = findBalance(summary.items, bob);
    const cofinance::db::BalanceRecord* carol_balance = findBalance(summary.items, carol);
    CHECK(alice_balance != nullptr && alice_balance->balance_cents == 7500);
    CHECK(bob_balance != nullptr && bob_balance->balance_cents == -4500);
    CHECK(carol_balance != nullptr && carol_balance->balance_cents == -3000);
    CHECK(!settlements.getBalances(dave, group.id, &summary, &error));
    CHECK(error.code == 1004);

    cofinance::service::GroupStats stats;
    CHECK(settlements.getStats(alice, group.id, cofinance::service::StatsInput{}, &stats,
                               &error));
    CHECK(stats.summary.total_expense_cents == 9000);
    CHECK(stats.summary.total_income_cents == 3000);
    CHECK(stats.summary.transaction_count == 2);
    CHECK(stats.members.size() == 3);
    const cofinance::db::CategoryStatRecord* expense_category =
        findCategory(stats.categories, 0, category_id);
    const cofinance::db::CategoryStatRecord* income_category =
        findCategory(stats.categories, 1, category_id);
    CHECK(expense_category != nullptr && expense_category->amount_cents == 9000);
    CHECK(income_category != nullptr && income_category->amount_cents == 3000);
    const cofinance::db::MemberStatRecord* bob_stats = findMember(stats.members, bob);
    CHECK(bob_stats != nullptr && bob_stats->paid_expense_cents == 0);
    CHECK(bob_stats != nullptr && bob_stats->paid_income_cents == 3000);
    CHECK(bob_stats != nullptr && bob_stats->share_expense_cents == 3000);
    CHECK(bob_stats != nullptr && bob_stats->share_income_cents == 1500);

    cofinance::service::StatsInput old_range;
    old_range.from = "2000-01-01";
    old_range.to = "2000-01-31";
    CHECK(settlements.getStats(alice, group.id, old_range, &stats, &error));
    CHECK(stats.summary.transaction_count == 0);
    CHECK(stats.members.size() == 3);
    cofinance::service::StatsInput invalid_range;
    invalid_range.from = "2026-02-30";
    CHECK(!settlements.getStats(alice, group.id, invalid_range, &stats, &error));
    CHECK(error.code == 1001);
    invalid_range.from = "2026-02-02";
    invalid_range.to = "2026-02-01";
    CHECK(!settlements.getStats(alice, group.id, invalid_range, &stats, &error));
    CHECK(error.code == 1001);

    std::printf("[case] 最少笔数建议与结算复核\n");
    std::vector<cofinance::service::SettlementSuggestion> suggestions;
    CHECK(settlements.suggestSettlements(alice, group.id, &suggestions, &error));
    CHECK(suggestions.size() == 2);
    CHECK(suggestions[0].from_user_id == bob && suggestions[0].to_user_id == alice);
    CHECK(suggestions[0].amount_cents == 4500);
    CHECK(suggestions[1].from_user_id == carol && suggestions[1].to_user_id == alice);
    CHECK(suggestions[1].amount_cents == 3000);
    CHECK(!settlements.suggestSettlements(dave, group.id, &suggestions, &error));
    CHECK(error.code == 1004);

    cofinance::db::SettlementRecord settlement;
    CHECK(!settlements.confirmSettlement(alice, group.id, bob, bob, 100, &settlement, &error));
    CHECK(error.code == 1001);
    CHECK(!settlements.confirmSettlement(alice, group.id, bob, alice, 0, &settlement, &error));
    CHECK(error.code == 1001);
    CHECK(!settlements.confirmSettlement(alice, group.id, dave, alice, 100, &settlement,
                                         &error));
    CHECK(error.code == 1001);
    CHECK(!settlements.confirmSettlement(alice, group.id, bob, alice, 4501, &settlement,
                                         &error));
    CHECK(error.code == 1015);
    CHECK(!settlements.confirmSettlement(alice, group.id, carol, bob, 100, &settlement,
                                         &error));
    CHECK(error.code == 1015);

    CHECK(settlements.confirmSettlement(alice, group.id, bob, alice, 4000, &settlement,
                                        &error));
    CHECK(settlement.id != 0 && settlement.status == 1 && settlement.created_by == alice);
    CHECK(settlement.amount_cents == 4000);
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM settlements WHERE group_id = ? AND status = 1",
                      {cofinance::db::SqlParam{group.id}}, &value, &db_error));
    CHECK(value == "1");
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    bob_balance = findBalance(summary.items, bob);
    CHECK(bob_balance != nullptr && bob_balance->settlement_balance_cents == 4000);
    CHECK(bob_balance != nullptr && bob_balance->balance_cents == -500);
    CHECK(!settlements.confirmSettlement(alice, group.id, bob, alice, 1000, &settlement,
                                         &error));
    CHECK(error.code == 1015);
    CHECK(settlements.confirmSettlement(alice, group.id, bob, alice, 500, &settlement, &error));
    CHECK(settlements.confirmSettlement(alice, group.id, carol, alice, 3000, &settlement,
                                        &error));
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    CHECK(summary.total_balance_cents == 0);
    for (const cofinance::db::BalanceRecord& balance : summary.items) {
        CHECK(balance.balance_cents == 0);
    }
    std::vector<cofinance::db::SettlementRecord> history;
    CHECK(settlements.listSettlements(alice, group.id, &history, &error));
    CHECK(history.size() == 3);
    CHECK(history.front().id > history.back().id);

    CHECK(groups.removeMember(alice, group.id, carol, &error));
    CHECK(groups.leaveGroup(bob, group.id, &error));

    std::printf("[case] 退出、移除与解散的余额闸门\n");
    cofinance::db::GroupRecord lifecycle;
    CHECK(groups.createGroup(alice, "P7 生命周期", &lifecycle, &error));
    groups_to_clean->push_back(lifecycle.id);
    joinGroup(groups, alice, lifecycle.id, bob, names[1]);
    joinGroup(groups, alice, lifecycle.id, carol, names[2]);
    CHECK(ledger.createTransaction(
        alice, lifecycle.id,
        transactionInput(0, 1000, alice, {participant(alice), participant(bob)}, category_id),
        &detail, &error));
    CHECK(!groups.leaveGroup(bob, lifecycle.id, &error));
    CHECK(error.code == 1015);
    CHECK(!groups.removeMember(alice, lifecycle.id, bob, &error));
    CHECK(error.code == 1015);
    CHECK(settlements.confirmSettlement(alice, lifecycle.id, bob, alice, 500, &settlement,
                                        &error));
    CHECK(groups.leaveGroup(bob, lifecycle.id, &error));
    CHECK(ledger.createTransaction(
        alice, lifecycle.id,
        transactionInput(0, 1000, alice, {participant(alice), participant(carol)},
                         category_id),
        &detail, &error));
    CHECK(!groups.dissolveGroup(carol, lifecycle.id, true, &error));
    CHECK(error.code == 1004);
    CHECK(!groups.dissolveGroup(alice, lifecycle.id, &error));
    CHECK(error.code == 1015);
    CHECK(groups.dissolveGroup(alice, lifecycle.id, true, &error));
    CHECK(queryScalar(connection, "SELECT status FROM finance_groups WHERE id = ?",
                      {cofinance::db::SqlParam{lifecycle.id}}, &value, &db_error));
    CHECK(value == "1");
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM messages WHERE group_id = ? AND sender_id = 0 "
                      "AND type = 1 AND content LIKE '%forced_dissolve%'",
                      {cofinance::db::SqlParam{lifecycle.id}}, &value, &db_error));
    CHECK(value == "1");
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM messages WHERE group_id = ? AND content LIKE ?",
                      {cofinance::db::SqlParam{lifecycle.id},
                       cofinance::db::SqlParam{"%" + std::to_string(carol) + "%"}},
                      &value, &db_error));
    CHECK(value == "1");

    cofinance::db::GroupRecord empty_group;
    CHECK(groups.createGroup(alice, "P7 零余额解散", &empty_group, &error));
    groups_to_clean->push_back(empty_group.id);
    CHECK(groups.dissolveGroup(alice, empty_group.id, &error));
}

void testConcurrentSettlement(cofinance::db::ConnectionPool& pool,
                              cofinance::db::MySqlConnection& connection,
                              const std::vector<std::uint64_t>& users,
                              const std::vector<std::string>& names,
                              std::vector<std::uint64_t>* groups_to_clean) {
    std::printf("[case] 并发结算只允许一笔成功\n");
    cofinance::service::GroupService groups(pool);
    cofinance::service::LedgerService ledger(pool);
    cofinance::service::SettlementService settlements(pool);
    cofinance::service::ServiceError error;
    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    cofinance::db::GroupRecord group;
    CHECK(groups.createGroup(alice, "P7 并发结算", &group, &error));
    groups_to_clean->push_back(group.id);
    joinGroup(groups, alice, group.id, bob, names[1]);
    cofinance::db::TransactionDetail detail;
    CHECK(ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 1000, alice, {participant(alice), participant(bob)}), &detail,
        &error));

    struct Attempt {
        bool ok = false;
        int code = 0;
    };
    Attempt first;
    Attempt second;
    std::thread worker_one([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::SettlementRecord record;
        first.ok = settlements.confirmSettlement(alice, group.id, bob, alice, 500, &record,
                                                 &local_error);
        first.code = local_error.code;
    });
    std::thread worker_two([&]() {
        cofinance::service::ServiceError local_error;
        cofinance::db::SettlementRecord record;
        second.ok = settlements.confirmSettlement(alice, group.id, bob, alice, 500, &record,
                                                  &local_error);
        second.code = local_error.code;
    });
    worker_one.join();
    worker_two.join();
    CHECK(first.ok != second.ok);
    CHECK((first.ok ? second.code : first.code) == 1015);
    std::string value;
    std::string db_error;
    CHECK(queryScalar(connection,
                      "SELECT COUNT(*) FROM settlements WHERE group_id = ? AND status = 1",
                      {cofinance::db::SqlParam{group.id}}, &value, &db_error));
    CHECK(value == "1");
    cofinance::service::BalanceSummary summary;
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    CHECK(summary.total_balance_cents == 0);
}

void testOptimalSettlement(cofinance::db::ConnectionPool& pool,
                           const std::vector<std::uint64_t>& users,
                           const std::vector<std::string>& names,
                           std::vector<std::uint64_t>* groups_to_clean) {
    std::printf("[case] 子集 DP 找到全局最少转账笔数\n");
    cofinance::service::GroupService groups(pool);
    cofinance::service::LedgerService ledger(pool);
    cofinance::service::SettlementService settlements(pool);
    cofinance::service::ServiceError error;
    const std::uint64_t alice = users[0];
    const std::uint64_t bob = users[1];
    const std::uint64_t carol = users[2];
    const std::uint64_t dave = users[3];
    const std::uint64_t eve = users[4];

    cofinance::db::GroupRecord group;
    CHECK(groups.createGroup(alice, "P7 最优结算", &group, &error));
    groups_to_clean->push_back(group.id);
    joinGroup(groups, alice, group.id, bob, names[1]);
    joinGroup(groups, alice, group.id, carol, names[2]);
    joinGroup(groups, alice, group.id, dave, names[3]);
    joinGroup(groups, alice, group.id, eve, names[4]);
    cofinance::db::TransactionDetail detail;
    CHECK(ledger.createTransaction(
        alice, group.id,
        transactionInput(0, 500, alice, {participant(carol, 400), participant(dave, 100)}, 0,
                         1),
        &detail, &error));
    CHECK(ledger.createTransaction(
        bob, group.id,
        transactionInput(0, 400, bob, {participant(dave, 200), participant(eve, 200)}, 0, 1),
        &detail, &error));

    cofinance::service::BalanceSummary summary;
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    CHECK(findBalance(summary.items, alice)->balance_cents == 500);
    CHECK(findBalance(summary.items, bob)->balance_cents == 400);
    CHECK(findBalance(summary.items, carol)->balance_cents == -400);
    CHECK(findBalance(summary.items, dave)->balance_cents == -300);
    CHECK(findBalance(summary.items, eve)->balance_cents == -200);

    std::vector<cofinance::service::SettlementSuggestion> suggestions;
    CHECK(settlements.suggestSettlements(alice, group.id, &suggestions, &error));
    CHECK(suggestions.size() == 3);
    for (const cofinance::service::SettlementSuggestion& suggestion : suggestions) {
        cofinance::db::SettlementRecord record;
        CHECK(settlements.confirmSettlement(alice, group.id, suggestion.from_user_id,
                                            suggestion.to_user_id,
                                            suggestion.amount_cents, &record, &error));
    }
    CHECK(settlements.getBalances(alice, group.id, &summary, &error));
    CHECK(summary.total_balance_cents == 0);
    for (const cofinance::db::BalanceRecord& balance : summary.items) {
        CHECK(balance.balance_cents == 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";
    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err) || !cfg.loadFile(siblingPath(config_path, "secrets.conf"),
                                                          &err)) {
        std::fprintf(stderr, "P7 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    cofinance::db::ConnectionPool pool;
    if (!pool.init(makeConfig(cfg), &err)) {
        std::fprintf(stderr, "P7 测试连接池初始化失败: %s\n", err.c_str());
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
        for (int i = 0; i < 5; ++i) {
            const std::string name = uniqueName("u", i);
            const std::uint64_t id = insertUser(connection, name, &db_error);
            if (id == 0) {
                std::fprintf(stderr, "创建 P7 测试用户失败: %s\n", db_error.c_str());
                return EXIT_FAILURE;
            }
            users.push_back(id);
            names.push_back(name);
        }
        testBalancesStatsAndSettlements(pool, connection, users, names, &groups);
        testConcurrentSettlement(pool, connection, users, names, &groups);
        testOptimalSettlement(pool, users, names, &groups);
        for (std::uint64_t group_id : groups) {
            hardDeleteGroup(connection, group_id);
        }
        for (std::uint64_t user_id : users) {
            hardDeleteUser(connection, user_id);
        }
    }
    if (g_failed != 0) {
        std::fprintf(stderr, "P7 结算测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P7 结算测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
