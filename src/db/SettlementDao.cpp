#include "cofinance/db/SettlementDao.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/MySqlConnection.h"

namespace cofinance {
namespace db {
namespace {

const char* const kSettlementColumns =
    "s.id, s.group_id, s.from_user_id, s.to_user_id, s.amount_cents, s.status, "
    "s.created_by, DATE_FORMAT(s.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(s.confirmed_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "fu.username, fu.nickname, tu.username, tu.nickname ";

const char* const kSettlementTables =
    "FROM settlements s JOIN users fu ON fu.id = s.from_user_id "
    "JOIN users tu ON tu.id = s.to_user_id ";

std::uint64_t parseUnsigned(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(*value, &consumed);
    if (consumed != value->size()) {
        throw std::runtime_error(std::string(field) + " 不是合法整数");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::int64_t parseSigned(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    std::size_t consumed = 0;
    const long long parsed = std::stoll(*value, &consumed);
    if (consumed != value->size()) {
        throw std::runtime_error(std::string(field) + " 不是合法整数");
    }
    return static_cast<std::int64_t>(parsed);
}

int parseInt(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    std::size_t consumed = 0;
    const int parsed = std::stoi(*value, &consumed);
    if (consumed != value->size()) {
        throw std::runtime_error(std::string(field) + " 不是合法整数");
    }
    return parsed;
}

std::string requiredText(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    return *value;
}

void appendTimeRange(std::string* sql, const std::optional<std::string>& from,
                     const std::optional<std::string>& to, std::vector<SqlParam>* params) {
    if (from.has_value()) {
        *sql += " AND t.created_at >= ?";
        params->push_back(SqlParam{*from});
    }
    if (to.has_value()) {
        *sql += " AND t.created_at <= ?";
        params->push_back(SqlParam{*to});
    }
}

SettlementRecord rowToSettlement(const SqlRow& row) {
    if (row.size() != 13) {
        throw std::runtime_error("结算查询结果列数不正确");
    }
    SettlementRecord settlement;
    settlement.id = parseUnsigned(row[0], "settlements.id");
    settlement.group_id = parseUnsigned(row[1], "settlements.group_id");
    settlement.from_user_id = parseUnsigned(row[2], "settlements.from_user_id");
    settlement.to_user_id = parseUnsigned(row[3], "settlements.to_user_id");
    settlement.amount_cents = parseSigned(row[4], "settlements.amount_cents");
    settlement.status = parseInt(row[5], "settlements.status");
    settlement.created_by = parseUnsigned(row[6], "settlements.created_by");
    settlement.created_at = requiredText(row[7], "settlements.created_at");
    if (row[8].has_value()) {
        settlement.confirmed_at = *row[8];
    }
    settlement.from_username = requiredText(row[9], "from_user.username");
    settlement.from_nickname = requiredText(row[10], "from_user.nickname");
    settlement.to_username = requiredText(row[11], "to_user.username");
    settlement.to_nickname = requiredText(row[12], "to_user.nickname");
    return settlement;
}

bool readDeltas(MySqlConnection& connection, const std::string& sql, std::uint64_t group_id,
                std::vector<UserDelta>* deltas, std::string* err) {
    if (deltas == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    deltas->clear();
    PreparedStatement statement(connection, sql, err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{group_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    for (;;) {
        SqlRow row;
        bool has_row = false;
        if (!statement.fetch(&row, &has_row, err)) {
            return false;
        }
        if (!has_row) {
            return true;
        }
        try {
            if (row.size() != 2) {
                throw std::runtime_error("余额聚合结果列数不正确");
            }
            UserDelta delta;
            delta.user_id = parseUnsigned(row[0], "delta.user_id");
            delta.delta_cents = parseSigned(row[1], "delta.delta_cents");
            deltas->push_back(delta);
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析余额聚合失败：") + e.what();
            }
            return false;
        }
    }
}

}  // namespace

SettlementDao::SettlementDao(MySqlConnection& connection) : connection_(connection) {}

bool SettlementDao::loadTransactionDeltas(std::uint64_t group_id,
                                          std::vector<UserDelta>* deltas,
                                          std::string* err) {
    const std::string sql =
        "SELECT d.user_id, SUM(d.delta_cents) FROM ("
        "SELECT t.payer_id AS user_id, "
        "CASE WHEN t.type = 0 THEN t.amount_cents ELSE -t.amount_cents END AS delta_cents "
        "FROM transactions t WHERE t.group_id = ? AND t.deleted_at IS NULL "
        "UNION ALL "
        "SELECT p.user_id, CASE WHEN t.type = 0 THEN -p.share_cents ELSE p.share_cents END "
        "FROM transaction_participants p JOIN transactions t ON t.id = p.transaction_id "
        "WHERE t.group_id = ? AND t.deleted_at IS NULL"
        ") d GROUP BY d.user_id ORDER BY d.user_id";
    return readDeltas(connection_, sql, group_id, deltas, err);
}

bool SettlementDao::loadSettlementDeltas(std::uint64_t group_id,
                                         std::vector<UserDelta>* deltas,
                                         std::string* err) {
    const std::string sql =
        "SELECT d.user_id, SUM(d.delta_cents) FROM ("
        "SELECT s.from_user_id AS user_id, s.amount_cents AS delta_cents FROM settlements s "
        "WHERE s.group_id = ? AND s.status = 1 "
        "UNION ALL "
        "SELECT s.to_user_id, -s.amount_cents FROM settlements s "
        "WHERE s.group_id = ? AND s.status = 1"
        ") d GROUP BY d.user_id ORDER BY d.user_id";
    return readDeltas(connection_, sql, group_id, deltas, err);
}

bool SettlementDao::summarize(std::uint64_t group_id,
                              const std::optional<std::string>& from,
                              const std::optional<std::string>& to,
                              SettlementSummaryRecord* summary, std::string* err) {
    if (summary == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    std::string sql =
        "SELECT COALESCE(SUM(CASE WHEN t.type = 0 THEN t.amount_cents ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN t.type = 1 THEN t.amount_cents ELSE 0 END), 0), COUNT(*) "
        "FROM transactions t WHERE t.group_id = ? AND t.deleted_at IS NULL";
    std::vector<SqlParam> params{SqlParam{group_id}};
    appendTimeRange(&sql, from, to, &params);
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row) {
        if (err != nullptr) {
            *err = "统计汇总没有返回结果";
        }
        return false;
    }
    try {
        if (row.size() != 3) {
            throw std::runtime_error("统计汇总结果列数不正确");
        }
        summary->total_expense_cents = parseSigned(row[0], "total_expense_cents");
        summary->total_income_cents = parseSigned(row[1], "total_income_cents");
        summary->transaction_count = parseUnsigned(row[2], "transaction_count");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析统计汇总失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool SettlementDao::listCategoryStats(std::uint64_t group_id,
                                      const std::optional<std::string>& from,
                                      const std::optional<std::string>& to,
                                      std::vector<CategoryStatRecord>* categories,
                                      std::string* err) {
    if (categories == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    categories->clear();
    std::string sql =
        "SELECT t.category_id, t.type, COALESCE(c.name, ''), SUM(t.amount_cents) "
        "FROM transactions t LEFT JOIN categories c ON c.id = t.category_id "
        "WHERE t.group_id = ? AND t.deleted_at IS NULL";
    std::vector<SqlParam> params{SqlParam{group_id}};
    appendTimeRange(&sql, from, to, &params);
    sql += " GROUP BY t.category_id, t.type, c.name ORDER BY t.type, SUM(t.amount_cents) DESC, "
           "t.category_id";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    for (;;) {
        SqlRow row;
        bool has_row = false;
        if (!statement.fetch(&row, &has_row, err)) {
            return false;
        }
        if (!has_row) {
            return true;
        }
        try {
            if (row.size() != 4) {
                throw std::runtime_error("分类统计结果列数不正确");
            }
            CategoryStatRecord category;
            category.category_id = parseUnsigned(row[0], "category_id");
            category.type = parseInt(row[1], "type");
            category.category_name = requiredText(row[2], "category_name");
            category.amount_cents = parseSigned(row[3], "amount_cents");
            categories->push_back(std::move(category));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析分类统计失败：") + e.what();
            }
            return false;
        }
    }
}

bool SettlementDao::listMemberStats(std::uint64_t group_id,
                                    const std::optional<std::string>& from,
                                    const std::optional<std::string>& to,
                                    std::vector<MemberStatRecord>* members, std::string* err) {
    if (members == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    members->clear();
    std::string events =
        "SELECT t.payer_id AS user_id, 0 AS entry_kind, t.type AS tx_type, "
        "t.amount_cents AS amount_cents, 0 AS share_cents "
        "FROM transactions t WHERE t.group_id = ? AND t.deleted_at IS NULL";
    std::vector<SqlParam> params{SqlParam{group_id}};
    appendTimeRange(&events, from, to, &params);
    events +=
        " UNION ALL SELECT p.user_id, 1, t.type, 0, p.share_cents "
        "FROM transaction_participants p JOIN transactions t ON t.id = p.transaction_id "
        "WHERE t.group_id = ? AND t.deleted_at IS NULL";
    params.push_back(SqlParam{group_id});
    appendTimeRange(&events, from, to, &params);
    const std::string sql =
        "SELECT u.id, u.username, u.nickname, u.avatar_url, "
        "COALESCE(SUM(CASE WHEN e.entry_kind = 0 AND e.tx_type = 0 "
        "THEN e.amount_cents ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN e.entry_kind = 0 AND e.tx_type = 1 "
        "THEN e.amount_cents ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN e.entry_kind = 1 AND e.tx_type = 0 "
        "THEN e.share_cents ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN e.entry_kind = 1 AND e.tx_type = 1 "
        "THEN e.share_cents ELSE 0 END), 0) "
        "FROM (" + events + ") e JOIN users u ON u.id = e.user_id "
        "GROUP BY u.id, u.username, u.nickname, u.avatar_url ORDER BY u.id";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    for (;;) {
        SqlRow row;
        bool has_row = false;
        if (!statement.fetch(&row, &has_row, err)) {
            return false;
        }
        if (!has_row) {
            return true;
        }
        try {
            if (row.size() != 8) {
                throw std::runtime_error("成员统计结果列数不正确");
            }
            MemberStatRecord member;
            member.user_id = parseUnsigned(row[0], "users.id");
            member.username = requiredText(row[1], "users.username");
            member.nickname = requiredText(row[2], "users.nickname");
            member.avatar_url = requiredText(row[3], "users.avatar_url");
            member.paid_expense_cents = parseSigned(row[4], "paid_expense_cents");
            member.paid_income_cents = parseSigned(row[5], "paid_income_cents");
            member.share_expense_cents = parseSigned(row[6], "share_expense_cents");
            member.share_income_cents = parseSigned(row[7], "share_income_cents");
            members->push_back(std::move(member));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析成员统计失败：") + e.what();
            }
            return false;
        }
    }
}

bool SettlementDao::insertSettlement(SettlementRecord& settlement, std::string* err) {
    PreparedStatement statement(
        connection_,
        "INSERT INTO settlements "
        "(group_id, from_user_id, to_user_id, amount_cents, status, created_by, confirmed_at) "
        "VALUES (?, ?, ?, ?, 1, ?, UTC_TIMESTAMP())",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{settlement.group_id}, SqlParam{settlement.from_user_id},
                         SqlParam{settlement.to_user_id}, SqlParam{settlement.amount_cents},
                         SqlParam{settlement.created_by}},
                        err) ||
        !statement.execute(err)) {
        return false;
    }
    settlement.id = statement.lastInsertId();
    settlement.status = 1;
    return true;
}

bool SettlementDao::findSettlement(std::uint64_t settlement_id, SettlementRecord* settlement,
                                   std::string* err) {
    if (settlement == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    const std::string sql = std::string("SELECT ") + kSettlementColumns + kSettlementTables +
                            "WHERE s.id = ?";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{settlement_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row) {
        if (err != nullptr) {
            *err = "结算记录不存在";
        }
        return false;
    }
    try {
        *settlement = rowToSettlement(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析结算记录失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool SettlementDao::listSettlements(std::uint64_t group_id,
                                    std::vector<SettlementRecord>* settlements,
                                    std::string* err) {
    if (settlements == nullptr) {
        if (err != nullptr) {
            *err = "SettlementDao 输出参数为空";
        }
        return false;
    }
    settlements->clear();
    const std::string sql = std::string("SELECT ") + kSettlementColumns + kSettlementTables +
                            "WHERE s.group_id = ? ORDER BY s.id DESC";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{group_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    for (;;) {
        SqlRow row;
        bool has_row = false;
        if (!statement.fetch(&row, &has_row, err)) {
            return false;
        }
        if (!has_row) {
            return true;
        }
        try {
            settlements->push_back(rowToSettlement(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析结算列表失败：") + e.what();
            }
            return false;
        }
    }
}

bool SettlementDao::insertSystemMessage(std::uint64_t group_id, const std::string& content,
                                        std::string* err) {
    PreparedStatement statement(
        connection_,
        "INSERT INTO messages (group_id, sender_id, type, content) VALUES (?, 0, 1, ?)", err);
    return statement.valid() &&
           statement.bind({SqlParam{group_id}, SqlParam{content}}, err) &&
           statement.execute(err);
}

}  // namespace db
}  // namespace cofinance
