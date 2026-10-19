#include "cofinance/db/UserDao.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cofinance/db/MySqlConnection.h"
#include "cofinance/db/PreparedStatement.h"

namespace cofinance {
namespace db {
namespace {

const char* const kUserColumns =
    "id, username, password_hash, nickname, avatar_url, status ";

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

User rowToUser(const SqlRow& row) {
    if (row.size() != 6) {
        throw std::runtime_error("用户查询结果列数不正确");
    }
    User user;
    user.id = parseUnsigned(row[0], "users.id");
    user.username = requiredText(row[1], "users.username");
    user.password_hash = requiredText(row[2], "users.password_hash");
    user.nickname = requiredText(row[3], "users.nickname");
    user.avatar_url = requiredText(row[4], "users.avatar_url");
    user.status = parseInt(row[5], "users.status");
    return user;
}

}  // namespace

UserDao::UserDao(MySqlConnection& connection) : connection_(connection) {}

bool UserDao::findById(std::uint64_t id, std::optional<User>* user, std::string* err) {
    const std::string sql = std::string("SELECT ") + kUserColumns + "FROM users WHERE id = ?";
    return readOne(sql, {SqlParam{id}}, user, err);
}

bool UserDao::findByUsername(const std::string& username, std::optional<User>* user,
                             std::string* err) {
    const std::string sql =
        std::string("SELECT ") + kUserColumns + "FROM users WHERE username = ?";
    return readOne(sql, {SqlParam{username}}, user, err);
}

bool UserDao::findActiveById(std::uint64_t id, std::optional<User>* user, std::string* err) {
    const std::string sql = std::string("SELECT ") + kUserColumns +
                            "FROM users WHERE id = ? AND status = 0 AND deleted_at IS NULL";
    return readOne(sql, {SqlParam{id}}, user, err);
}

bool UserDao::findActiveByUsername(const std::string& username, std::optional<User>* user,
                                   std::string* err) {
    const std::string sql = std::string("SELECT ") + kUserColumns +
                            "FROM users WHERE username = ? AND status = 0 AND deleted_at IS NULL";
    return readOne(sql, {SqlParam{username}}, user, err);
}

bool UserDao::insert(User& user, std::string* err, bool* duplicate) {
    if (duplicate != nullptr) {
        *duplicate = false;
    }
    PreparedStatement statement(
        connection_,
        "INSERT INTO users (username, password_hash, nickname, avatar_url, status) "
        "VALUES (?, ?, ?, ?, ?)",
        err);
    if (!statement.valid()) {
        return false;
    }
    if (!statement.bind({SqlParam{user.username}, SqlParam{user.password_hash},
                         SqlParam{user.nickname}, SqlParam{user.avatar_url},
                         SqlParam{static_cast<std::int64_t>(user.status)}},
                        err) ||
        !statement.execute(err)) {
        if (duplicate != nullptr) {
            *duplicate = statement.lastErrorCode() == 1062;
        }
        return false;
    }
    user.id = statement.lastInsertId();
    return true;
}

bool UserDao::updateProfile(std::uint64_t id, const std::string& nickname,
                            const std::string& avatar_url, std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE users SET nickname = ?, avatar_url = ? "
        "WHERE id = ? AND status = 0 AND deleted_at IS NULL",
        err);
    return statement.valid() &&
           statement.bind({SqlParam{nickname}, SqlParam{avatar_url}, SqlParam{id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool UserDao::updatePasswordHashIfCurrent(std::uint64_t id, const std::string& expected_hash,
                                          const std::string& new_hash, bool* updated,
                                          std::string* err) {
    if (updated != nullptr) {
        *updated = false;
    }
    PreparedStatement statement(
        connection_,
        "UPDATE users SET password_hash = ? WHERE id = ? AND password_hash = ? "
        "AND status = 0 AND deleted_at IS NULL",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{new_hash}, SqlParam{id}, SqlParam{expected_hash}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    if (updated != nullptr) {
        *updated = statement.affectedRows() == 1;
    }
    return true;
}

bool UserDao::softDelete(std::uint64_t id, std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE users SET status = 2, deleted_at = UTC_TIMESTAMP() "
        "WHERE id = ? AND deleted_at IS NULL",
        err);
    return statement.valid() && statement.bind({SqlParam{id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool UserDao::searchActiveByUsername(const std::string& query, std::uint64_t exclude_id,
                                     std::vector<User>* users, std::string* err) {
    if (users == nullptr) {
        if (err != nullptr) {
            *err = "UserDao 搜索输出参数为空";
        }
        return false;
    }
    users->clear();

    std::string pattern;
    pattern.reserve(query.size() + 2);
    pattern.push_back('%');
    for (char c : query) {
        if (c == '!' || c == '%' || c == '_') {
            pattern.push_back('!');
        }
        pattern.push_back(c);
    }
    pattern.push_back('%');

    const std::string sql = std::string("SELECT ") + kUserColumns +
                            "FROM users WHERE username LIKE ? ESCAPE '!' AND id <> ? "
                            "AND status = 0 AND deleted_at IS NULL ORDER BY username LIMIT 20";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{pattern}, SqlParam{exclude_id}}, err) ||
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
            users->push_back(rowToUser(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析用户行失败：") + e.what();
            }
            return false;
        }
    }
}

bool UserDao::readOne(const std::string& sql, const std::vector<SqlParam>& params,
                      std::optional<User>* user, std::string* err) {
    if (user == nullptr) {
        if (err != nullptr) {
            *err = "UserDao 输出参数为空";
        }
        return false;
    }
    user->reset();

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
        return true;
    }
    try {
        *user = rowToUser(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析用户行失败：") + e.what();
        }
        return false;
    }
    return true;
}

}  // namespace db
}  // namespace cofinance
