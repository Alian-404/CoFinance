#include "cofinance/db/MessageDao.h"

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

const char* const kMessageColumns =
    "m.id, m.group_id, m.sender_id, m.type, m.content, m.client_msg_id, "
    "DATE_FORMAT(m.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "COALESCE(u.username, ''), COALESCE(u.nickname, ''), COALESCE(u.avatar_url, '') ";

const char* const kMessageTables =
    "FROM messages m LEFT JOIN users u ON u.id = m.sender_id ";

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

MessageRecord rowToMessage(const SqlRow& row) {
    if (row.size() != 10) {
        throw std::runtime_error("消息查询结果列数不正确");
    }
    MessageRecord message;
    message.id = parseUnsigned(row[0], "messages.id");
    message.group_id = parseUnsigned(row[1], "messages.group_id");
    message.sender_id = parseUnsigned(row[2], "messages.sender_id");
    message.type = parseInt(row[3], "messages.type");
    message.content = requiredText(row[4], "messages.content");
    if (row[5].has_value()) {
        message.client_msg_id = *row[5];
    }
    message.created_at = requiredText(row[6], "messages.created_at");
    message.sender_username = requiredText(row[7], "sender_username");
    message.sender_nickname = requiredText(row[8], "sender_nickname");
    message.sender_avatar_url = requiredText(row[9], "sender_avatar_url");
    return message;
}

}  // namespace

MessageDao::MessageDao(MySqlConnection& connection) : connection_(connection) {}

bool MessageDao::insert(MessageRecord& message, std::string* err, bool* duplicate) {
    if (duplicate != nullptr) {
        *duplicate = false;
    }
    const SqlParam client_id = message.client_msg_id.has_value()
                                   ? SqlParam{*message.client_msg_id}
                                   : SqlParam{nullptr};
    PreparedStatement statement(
        connection_,
        "INSERT INTO messages (group_id, sender_id, type, content, client_msg_id) "
        "VALUES (?, ?, ?, ?, ?)",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{message.group_id}, SqlParam{message.sender_id},
                         SqlParam{static_cast<std::int64_t>(message.type)},
                         SqlParam{message.content}, client_id},
                        err) ||
        !statement.execute(err)) {
        if (duplicate != nullptr) {
            *duplicate = statement.lastErrorCode() == 1062;
        }
        return false;
    }
    message.id = statement.lastInsertId();
    return true;
}

bool MessageDao::findById(std::uint64_t message_id,
                          std::optional<MessageRecord>* message, std::string* err) {
    if (message == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    message->reset();
    const std::string sql = std::string("SELECT ") + kMessageColumns + kMessageTables +
                            "WHERE m.id = ?";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{message_id}}, err) ||
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
        *message = rowToMessage(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析消息行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool MessageDao::findBySenderClient(std::uint64_t sender_id, const std::string& client_msg_id,
                                    std::optional<MessageRecord>* message,
                                    std::string* err) {
    if (message == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    message->reset();
    const std::string sql = std::string("SELECT ") + kMessageColumns + kMessageTables +
                            "WHERE m.sender_id = ? AND m.client_msg_id = ? "
                            "ORDER BY m.id ASC LIMIT 1";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{sender_id}, SqlParam{client_msg_id}}, err) ||
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
        *message = rowToMessage(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析幂等消息失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool MessageDao::list(std::uint64_t group_id, const MessageFilter& filter,
                      std::vector<MessageRecord>* messages, bool* has_more,
                      std::string* err) {
    if (messages == nullptr || has_more == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    messages->clear();
    *has_more = false;

    std::string sql = std::string("SELECT ") + kMessageColumns + kMessageTables +
                      "WHERE m.group_id = ?";
    std::vector<SqlParam> params{SqlParam{group_id}};
    if (filter.after_id.has_value()) {
        sql += " AND m.id > ? ORDER BY m.id ASC LIMIT ?";
        params.push_back(SqlParam{*filter.after_id});
    } else if (filter.cursor.has_value()) {
        sql += " AND m.id < ? ORDER BY m.id DESC LIMIT ?";
        params.push_back(SqlParam{*filter.cursor});
    } else {
        sql += " ORDER BY m.id DESC LIMIT ?";
    }
    params.push_back(SqlParam{static_cast<std::int64_t>(filter.limit + 1)});

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
            break;
        }
        try {
            messages->push_back(rowToMessage(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析消息列表失败：") + e.what();
            }
            return false;
        }
    }
    if (messages->size() > filter.limit) {
        messages->resize(filter.limit);
        *has_more = true;
    }
    return true;
}

bool MessageDao::maxId(std::uint64_t group_id, std::uint64_t* max_id, std::string* err) {
    if (max_id == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    PreparedStatement statement(
        connection_, "SELECT COALESCE(MAX(id), 0) FROM messages WHERE group_id = ?", err);
    if (!statement.valid() || !statement.bind({SqlParam{group_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err) || !has_row || row.size() != 1) {
        if (err != nullptr && err->empty()) {
            *err = "消息最大 id 查询没有返回一行一列";
        }
        return false;
    }
    try {
        *max_id = parseUnsigned(row[0], "MAX(messages.id)");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析消息最大 id 失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool MessageDao::countAfter(std::uint64_t group_id, std::uint64_t after_id,
                            std::uint64_t* count, std::string* err) {
    if (count == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    PreparedStatement statement(
        connection_, "SELECT COUNT(*) FROM messages WHERE group_id = ? AND id > ?", err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{after_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err) || !has_row || row.size() != 1) {
        if (err != nullptr && err->empty()) {
            *err = "未读数查询没有返回一行一列";
        }
        return false;
    }
    try {
        *count = parseUnsigned(row[0], "COUNT(messages.id)");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析未读数失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool MessageDao::updateReadWatermark(std::uint64_t group_id, std::uint64_t user_id,
                                     std::uint64_t message_id, std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE group_members SET last_read_message_id = GREATEST(last_read_message_id, ?) "
        "WHERE group_id = ? AND user_id = ? AND status = 1",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{message_id}, SqlParam{group_id}, SqlParam{user_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    // GREATEST 可能让新值与旧值相同，此时 affectedRows=0 并不代表成员不存在。
    // 调用方随后用 readWatermark() 校验成员与最终水位线。
    return true;
}

bool MessageDao::readWatermark(std::uint64_t group_id, std::uint64_t user_id,
                               std::uint64_t* message_id, std::string* err) {
    if (message_id == nullptr) {
        if (err != nullptr) {
            *err = "MessageDao 输出参数为空";
        }
        return false;
    }
    PreparedStatement statement(
        connection_,
        "SELECT last_read_message_id FROM group_members "
        "WHERE group_id = ? AND user_id = ? AND status = 1",
        err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{user_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err) || !has_row || row.size() != 1) {
        if (err != nullptr && err->empty()) {
            *err = "成员已读水位线不存在";
        }
        return false;
    }
    try {
        *message_id = parseUnsigned(row[0], "group_members.last_read_message_id");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析已读水位线失败：") + e.what();
        }
        return false;
    }
    return true;
}

}  // namespace db
}  // namespace cofinance
