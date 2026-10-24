#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/PreparedStatement.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

struct MessageRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t sender_id = 0;
    int type = 0;
    std::string content;
    std::optional<std::string> client_msg_id;
    std::string created_at;
    std::string sender_username;
    std::string sender_nickname;
    std::string sender_avatar_url;
};

struct MessageFilter {
    std::optional<std::uint64_t> cursor;
    std::optional<std::uint64_t> after_id;
    std::uint64_t limit = 20;
};

class MessageDao : private NonCopyable {
public:
    explicit MessageDao(MySqlConnection& connection);

    bool insert(MessageRecord& message, std::string* err, bool* duplicate = nullptr);
    bool findById(std::uint64_t message_id, std::optional<MessageRecord>* message,
                  std::string* err);
    bool findBySenderClient(std::uint64_t sender_id, const std::string& client_msg_id,
                            std::optional<MessageRecord>* message, std::string* err);
    bool list(std::uint64_t group_id, const MessageFilter& filter,
              std::vector<MessageRecord>* messages, bool* has_more, std::string* err);

    bool maxId(std::uint64_t group_id, std::uint64_t* max_id, std::string* err);
    bool countAfter(std::uint64_t group_id, std::uint64_t after_id, std::uint64_t* count,
                    std::string* err);
    bool updateReadWatermark(std::uint64_t group_id, std::uint64_t user_id,
                             std::uint64_t message_id, std::string* err);
    bool readWatermark(std::uint64_t group_id, std::uint64_t user_id,
                       std::uint64_t* message_id, std::string* err);

private:
    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
