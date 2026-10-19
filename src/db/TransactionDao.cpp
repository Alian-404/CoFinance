#include "cofinance/db/TransactionDao.h"

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

const char* const kJoinedColumns =
    "t.id, t.group_id, t.payer_id, t.type, t.amount_cents, t.currency, "
    "t.category_id, t.note, t.split_type, t.client_token, t.created_by, "
    "DATE_FORMAT(t.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(t.updated_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "COALESCE(c.name, ''), COALESCE(p.nickname, ''), (t.deleted_at IS NOT NULL) ";

const char* const kJoinedTables =
    "FROM transactions t LEFT JOIN categories c ON c.id = t.category_id "
    "LEFT JOIN users p ON p.id = t.payer_id ";

const char* const kLockedColumns =
    "t.id, t.group_id, t.payer_id, t.type, t.amount_cents, t.currency, "
    "t.category_id, t.note, t.split_type, t.client_token, t.created_by, "
    "DATE_FORMAT(t.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(t.updated_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "'', '', (t.deleted_at IS NOT NULL) ";

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

TransactionRecord rowToTransaction(const SqlRow& row) {
    if (row.size() != 16) {
        throw std::runtime_error("交易查询结果列数不正确");
    }
    TransactionRecord transaction;
    transaction.id = parseUnsigned(row[0], "transactions.id");
    transaction.group_id = parseUnsigned(row[1], "transactions.group_id");
    transaction.payer_id = parseUnsigned(row[2], "transactions.payer_id");
    transaction.type = parseInt(row[3], "transactions.type");
    transaction.amount_cents = parseSigned(row[4], "transactions.amount_cents");
    transaction.currency = requiredText(row[5], "transactions.currency");
    transaction.category_id = parseUnsigned(row[6], "transactions.category_id");
    transaction.note = requiredText(row[7], "transactions.note");
    transaction.split_type = parseInt(row[8], "transactions.split_type");
    if (row[9].has_value()) {
        transaction.client_token = *row[9];
    }
    transaction.created_by = parseUnsigned(row[10], "transactions.created_by");
    transaction.created_at = requiredText(row[11], "transactions.created_at");
    transaction.updated_at = requiredText(row[12], "transactions.updated_at");
    transaction.category_name = requiredText(row[13], "category_name");
    transaction.payer_nickname = requiredText(row[14], "payer_nickname");
    transaction.deleted = parseInt(row[15], "transactions.deleted") != 0;
    return transaction;
}

ParticipantRecord rowToParticipant(const SqlRow& row) {
    if (row.size() != 7) {
        throw std::runtime_error("参与人查询结果列数不正确");
    }
    ParticipantRecord participant;
    participant.id = parseUnsigned(row[0], "transaction_participants.id");
    participant.transaction_id = parseUnsigned(row[1], "transaction_participants.transaction_id");
    participant.user_id = parseUnsigned(row[2], "transaction_participants.user_id");
    participant.share_cents = parseSigned(row[3], "transaction_participants.share_cents");
    participant.username = requiredText(row[4], "users.username");
    participant.nickname = requiredText(row[5], "users.nickname");
    participant.avatar_url = requiredText(row[6], "users.avatar_url");
    return participant;
}

}  // namespace

TransactionDao::TransactionDao(MySqlConnection& connection) : connection_(connection) {}

bool TransactionDao::insert(TransactionRecord& transaction, std::string* err, bool* duplicate) {
    if (duplicate != nullptr) {
        *duplicate = false;
    }
    const SqlParam token = transaction.client_token.has_value()
                               ? SqlParam{*transaction.client_token}
                               : SqlParam{nullptr};
    PreparedStatement statement(
        connection_,
        "INSERT INTO transactions "
        "(group_id, payer_id, type, amount_cents, currency, category_id, note, split_type, "
        "client_token, created_by) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{transaction.group_id}, SqlParam{transaction.payer_id},
                         SqlParam{static_cast<std::int64_t>(transaction.type)},
                         SqlParam{transaction.amount_cents}, SqlParam{transaction.currency},
                         SqlParam{transaction.category_id}, SqlParam{transaction.note},
                         SqlParam{static_cast<std::int64_t>(transaction.split_type)}, token,
                         SqlParam{transaction.created_by}},
                        err) ||
        !statement.execute(err)) {
        if (duplicate != nullptr) {
            *duplicate = statement.lastErrorCode() == 1062;
        }
        return false;
    }
    transaction.id = statement.lastInsertId();
    transaction.deleted = false;
    return true;
}

bool TransactionDao::updateHeader(const TransactionRecord& transaction, std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE transactions SET payer_id = ?, type = ?, amount_cents = ?, currency = ?, "
        "category_id = ?, note = ?, split_type = ? "
        "WHERE id = ? AND group_id = ? AND deleted_at IS NULL",
        err);
    return statement.valid() &&
           statement.bind({SqlParam{transaction.payer_id},
                           SqlParam{static_cast<std::int64_t>(transaction.type)},
                           SqlParam{transaction.amount_cents}, SqlParam{transaction.currency},
                           SqlParam{transaction.category_id}, SqlParam{transaction.note},
                           SqlParam{static_cast<std::int64_t>(transaction.split_type)},
                           SqlParam{transaction.id}, SqlParam{transaction.group_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool TransactionDao::softDelete(std::uint64_t id, std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE transactions SET deleted_at = UTC_TIMESTAMP() "
                                "WHERE id = ? AND deleted_at IS NULL",
                                err);
    return statement.valid() && statement.bind({SqlParam{id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool TransactionDao::findHeaderById(std::uint64_t group_id, std::uint64_t transaction_id,
                                    bool for_update,
                                    std::optional<TransactionRecord>* transaction,
                                    std::string* err) {
    if (transaction == nullptr) {
        if (err != nullptr) {
            *err = "TransactionDao 输出参数为空";
        }
        return false;
    }
    transaction->reset();
    std::string sql;
    if (for_update) {
        sql = std::string("SELECT ") + kLockedColumns +
              "FROM transactions t WHERE t.group_id = ? AND t.id = ? "
              "AND t.deleted_at IS NULL FOR UPDATE";
    } else {
        sql = std::string("SELECT ") + kJoinedColumns + kJoinedTables +
              "WHERE t.group_id = ? AND t.id = ? AND t.deleted_at IS NULL";
    }
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{transaction_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row) {
        return true;
    }
    try {
        *transaction = rowToTransaction(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析交易行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool TransactionDao::findByCreatorToken(std::uint64_t created_by,
                                        const std::string& client_token,
                                        std::optional<TransactionRecord>* transaction,
                                        std::string* err) {
    if (transaction == nullptr) {
        if (err != nullptr) {
            *err = "TransactionDao 输出参数为空";
        }
        return false;
    }
    transaction->reset();
    const std::string sql = std::string("SELECT ") + kJoinedColumns + kJoinedTables +
                            "WHERE t.created_by = ? AND t.client_token = ? "
                            "ORDER BY t.id ASC LIMIT 1";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{created_by}, SqlParam{client_token}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row) {
        return true;
    }
    try {
        *transaction = rowToTransaction(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析幂等交易失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool TransactionDao::list(std::uint64_t group_id, const TransactionFilter& filter,
                          std::vector<TransactionRecord>* transactions, std::string* err) {
    if (transactions == nullptr) {
        if (err != nullptr) {
            *err = "TransactionDao 输出参数为空";
        }
        return false;
    }
    transactions->clear();
    std::string sql = std::string("SELECT ") + kJoinedColumns + kJoinedTables +
                      "WHERE t.group_id = ? AND t.deleted_at IS NULL";
    std::vector<SqlParam> params;
    params.push_back(SqlParam{group_id});
    if (filter.cursor.has_value()) {
        sql += " AND t.id < ?";
        params.push_back(SqlParam{*filter.cursor});
    }
    if (filter.category_id.has_value()) {
        sql += " AND t.category_id = ?";
        params.push_back(SqlParam{*filter.category_id});
    }
    if (filter.payer_id.has_value()) {
        sql += " AND t.payer_id = ?";
        params.push_back(SqlParam{*filter.payer_id});
    }
    if (filter.from.has_value()) {
        sql += " AND t.created_at >= ?";
        params.push_back(SqlParam{*filter.from});
    }
    if (filter.to.has_value()) {
        sql += " AND t.created_at <= ?";
        params.push_back(SqlParam{*filter.to});
    }
    sql += " ORDER BY t.id DESC LIMIT ?";
    params.push_back(SqlParam{static_cast<std::int64_t>(filter.limit)});

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
            transactions->push_back(rowToTransaction(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析交易列表失败：") + e.what();
            }
            return false;
        }
    }
}

bool TransactionDao::listParticipants(std::uint64_t transaction_id,
                                      std::vector<ParticipantRecord>* participants,
                                      std::string* err) {
    if (participants == nullptr) {
        if (err != nullptr) {
            *err = "TransactionDao 输出参数为空";
        }
        return false;
    }
    participants->clear();
    PreparedStatement statement(
        connection_,
        "SELECT p.id, p.transaction_id, p.user_id, p.share_cents, "
        "u.username, u.nickname, u.avatar_url "
        "FROM transaction_participants p JOIN users u ON u.id = p.user_id "
        "WHERE p.transaction_id = ? ORDER BY p.id ASC",
        err);
    if (!statement.valid() || !statement.bind({SqlParam{transaction_id}}, err) ||
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
            participants->push_back(rowToParticipant(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析参与人失败：") + e.what();
            }
            return false;
        }
    }
}

bool TransactionDao::insertParticipant(std::uint64_t transaction_id, std::uint64_t user_id,
                                       std::int64_t share_cents, std::string* err) {
    PreparedStatement statement(connection_,
                                "INSERT INTO transaction_participants "
                                "(transaction_id, user_id, share_cents) VALUES (?, ?, ?)",
                                err);
    return statement.valid() &&
           statement.bind({SqlParam{transaction_id}, SqlParam{user_id},
                           SqlParam{share_cents}}, err) &&
           statement.execute(err);
}

bool TransactionDao::deleteParticipants(std::uint64_t transaction_id, std::string* err) {
    PreparedStatement statement(connection_,
                                "DELETE FROM transaction_participants WHERE transaction_id = ?",
                                err);
    return statement.valid() && statement.bind({SqlParam{transaction_id}}, err) &&
           statement.execute(err);
}

}  // namespace db
}  // namespace cofinance
