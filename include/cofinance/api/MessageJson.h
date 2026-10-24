#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "cofinance/db/MessageDao.h"

namespace cofinance {
namespace api {
namespace detail {

inline nlohmann::json messageJson(const db::MessageRecord& message) {
    nlohmann::json client_msg_id = nullptr;
    if (message.client_msg_id.has_value()) {
        client_msg_id = *message.client_msg_id;
    }
    std::string sender_name = message.sender_nickname;
    if (sender_name.empty()) {
        sender_name = message.sender_username;
    }
    if (message.sender_id == 0) {
        sender_name = "系统";
    }
    return nlohmann::json{{"id", message.id},
                          {"group_id", message.group_id},
                          {"sender_id", message.sender_id},
                          {"sender_username", message.sender_username},
                          {"sender_nickname", message.sender_nickname},
                          {"sender_avatar_url", message.sender_avatar_url},
                          {"sender_name", std::move(sender_name)},
                          {"type", message.type},
                          {"content", message.content},
                          {"client_msg_id", std::move(client_msg_id)},
                          {"created_at", message.created_at}};
}

}  // namespace detail
}  // namespace api
}  // namespace cofinance
