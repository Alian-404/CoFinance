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

bool UserDao::insert(User& user, std::string* err) {
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
        return false;
    }
    user.id = statement.lastInsertId();
    return true;
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
