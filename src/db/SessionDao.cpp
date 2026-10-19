#include "cofinance/db/SessionDao.h"

#include <optional>
#include <stdexcept>
#include <string>

#include "cofinance/db/MySqlConnection.h"
#include "cofinance/db/PreparedStatement.h"

namespace cofinance {
namespace db {

SessionDao::SessionDao(MySqlConnection& connection) : connection_(connection) {}

bool SessionDao::create(std::uint64_t user_id, const std::string& token_hash, std::string* err) {
    PreparedStatement statement(
        connection_,
        "INSERT INTO sessions (token_hash, user_id, created_at, expires_at, last_seen_at) "
        "VALUES (?, ?, UTC_TIMESTAMP(), DATE_ADD(UTC_TIMESTAMP(), INTERVAL 7 DAY), "
        "UTC_TIMESTAMP())",
        err);
    return statement.valid() &&
           statement.bind({SqlParam{token_hash}, SqlParam{user_id}}, err) &&
           statement.execute(err);
}

bool SessionDao::find(const std::string& token_hash,
                      std::optional<SessionRecord>* session, std::string* err) {
    if (session == nullptr) {
        if (err != nullptr) {
            *err = "SessionDao 输出参数为空";
        }
        return false;
    }
    session->reset();

    PreparedStatement statement(
        connection_,
        "SELECT user_id, expires_at <= UTC_TIMESTAMP() AS expired, "
        "last_seen_at < DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 HOUR) AS needs_renewal "
        "FROM sessions WHERE token_hash = ?",
        err);
    if (!statement.valid() || !statement.bind({SqlParam{token_hash}}, err) ||
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
        if (row.size() != 3 || !row[0].has_value() || !row[1].has_value() ||
            !row[2].has_value()) {
            throw std::runtime_error("sessions 查询结果列非法");
        }
        SessionRecord out;
        out.user_id = static_cast<std::uint64_t>(std::stoull(*row[0]));
        out.expired = *row[1] != "0";
        out.needs_renewal = *row[2] != "0";
        *session = out;
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析 session 行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool SessionDao::touch(const std::string& token_hash, bool* touched, std::string* err) {
    if (touched != nullptr) {
        *touched = false;
    }
    PreparedStatement statement(
        connection_,
        "UPDATE sessions SET last_seen_at = UTC_TIMESTAMP(), "
        "expires_at = IF(expires_at < DATE_ADD(UTC_TIMESTAMP(), INTERVAL 6 DAY), "
        "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 7 DAY), expires_at) "
        "WHERE token_hash = ? AND expires_at > UTC_TIMESTAMP()",
        err);
    if (!statement.valid() || !statement.bind({SqlParam{token_hash}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    if (touched != nullptr) {
        *touched = statement.affectedRows() > 0;
    }
    return true;
}

bool SessionDao::remove(const std::string& token_hash, bool* removed, std::string* err) {
    if (removed != nullptr) {
        *removed = false;
    }
    PreparedStatement statement(connection_, "DELETE FROM sessions WHERE token_hash = ?", err);
    if (!statement.valid() || !statement.bind({SqlParam{token_hash}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    if (removed != nullptr) {
        *removed = statement.affectedRows() > 0;
    }
    return true;
}

bool SessionDao::removeAll(std::uint64_t user_id, std::string* err) {
    PreparedStatement statement(connection_, "DELETE FROM sessions WHERE user_id = ?", err);
    return statement.valid() && statement.bind({SqlParam{user_id}}, err) &&
           statement.execute(err);
}

bool SessionDao::removeOthers(std::uint64_t user_id, const std::string& keep_token_hash,
                              std::string* err) {
    PreparedStatement statement(
        connection_, "DELETE FROM sessions WHERE user_id = ? AND token_hash <> ?", err);
    return statement.valid() &&
           statement.bind({SqlParam{user_id}, SqlParam{keep_token_hash}}, err) &&
           statement.execute(err);
}

}  // namespace db
}  // namespace cofinance
