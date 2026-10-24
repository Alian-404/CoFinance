#include "cofinance/api/WebSocketApi.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/api/MessageJson.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/net/TcpConnection.h"
#include "cofinance/service/ChatService.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Logger.h"
#include "cofinance/ws/WebSocketHandshake.h"

namespace cofinance {
namespace api {
namespace {

using json = support::json;
using http::HttpRequest;
using http::HttpResponse;
using service::AuthContext;
using service::ChatService;
using service::ServiceError;
using service::UserService;

http::UpgradeDecision reject(int status, int code, const std::string& message) {
    http::UpgradeDecision decision;
    decision.rejection = support::jsonResult(status, code, message);
    return decision;
}

void sendJson(const std::shared_ptr<ws::WebSocketSession>& session, const json& value) {
    if (session) {
        session->sendText(value.dump());
    }
}

void sendError(const std::shared_ptr<ws::WebSocketSession>& session, const char* request_type,
               int code, const std::string& message) {
    sendJson(session, json{{"type", "error"},
                           {"request", request_type},
                           {"code", code},
                           {"message", message}});
}

void handleClientText(const std::shared_ptr<ws::WebSocketSession>& session,
                      ChatService& chat, std::uint64_t user_id, std::uint64_t group_id,
                      const std::string& text) {
    json message = json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        sendError(session, "unknown", 1001, "消息必须是 JSON 对象");
        return;
    }
    const auto type_it = message.find("type");
    if (type_it == message.end() || !type_it->is_string()) {
        sendError(session, "unknown", 1001, "缺少字符串字段: type");
        return;
    }
    const std::string type = type_it->get<std::string>();
    if (type == "send") {
        const auto content_it = message.find("content");
        if (content_it == message.end() || !content_it->is_string()) {
            sendError(session, "send", 1001, "缺少字符串字段: content");
            return;
        }
        std::optional<std::string> client_msg_id;
        const auto client_it = message.find("client_msg_id");
        if (client_it != message.end() && !client_it->is_null()) {
            if (!client_it->is_string()) {
                sendError(session, "send", 1001, "client_msg_id 必须是字符串");
                return;
            }
            client_msg_id = client_it->get<std::string>();
        }
        db::MessageRecord created;
        ServiceError error;
        if (!chat.sendMessage(user_id, group_id, content_it->get<std::string>(),
                              client_msg_id, &created, &error)) {
            sendError(session, "send", error.code, error.message);
            return;
        }
        json client_id = nullptr;
        if (client_msg_id.has_value()) {
            client_id = *client_msg_id;
        }
        sendJson(session, json{{"type", "ack"},
                               {"client_msg_id", std::move(client_id)},
                               {"message_id", created.id}});
        return;
    }
    if (type == "read") {
        const auto id_it = message.find("last_read_message_id");
        std::int64_t raw_id = -1;
        if (id_it == message.end() || !support::parseInt64Value(*id_it, &raw_id) || raw_id < 0) {
            sendError(session, "read", 1001, "last_read_message_id 必须是非负整数");
            return;
        }
        ServiceError error;
        service::UnreadState state;
        if (!chat.markRead(user_id, group_id, static_cast<std::uint64_t>(raw_id), &state,
                           &error)) {
            sendError(session, "read", error.code, error.message);
            return;
        }
        sendJson(session, json{{"type", "read"},
                               {"data",
                                json{{"last_read_message_id", state.last_read_message_id},
                                     {"unread_count", state.unread_count},
                                     {"max_message_id", state.max_message_id}}}});
        return;
    }
    if (type == "ping") {
        sendJson(session, json{{"type", "pong"}});
        return;
    }
    sendError(session, type.c_str(), 1001, "未知的消息类型");
}

}  // namespace

void WebSocketApi::registerRoutes(http::HttpServer& server, UserService& users,
                                  ChatService& chat,
                                  const ws::WebSocketSession::Config& config) {
    server.addUpgradeRoute(
        "/api/ws",
        [&users, &chat, config](HttpRequest& request,
                                const http::UpgradeContext& context) -> http::UpgradeDecision {
            std::string accept;
            int status = 400;
            std::string handshake_error;
            if (!ws::validateHandshake(request, &accept, &status, &handshake_error)) {
                http::UpgradeDecision decision =
                    reject(status, 1001, "WebSocket 握手失败: " + handshake_error);
                if (status == 426) {
                    decision.rejection.setHeader("Sec-WebSocket-Version", "13");
                }
                return decision;
            }

            std::string token;
            if (!support::queryValue(request.query, "token", &token) || token.empty() ||
                token.size() > 256) {
                return reject(401, 1002, "未登录或 token 无效");
            }
            std::string raw_group_id;
            std::uint64_t group_id = 0;
            if (!support::queryValue(request.query, "group_id", &raw_group_id) ||
                !support::parseUInt64(raw_group_id, &group_id)) {
                return reject(400, 1001, "group_id 必须是正整数");
            }

            AuthContext auth;
            ServiceError auth_error;
            if (!users.authenticate(token, &auth, &auth_error)) {
                return reject(auth_error.http_status, auth_error.code, auth_error.message);
            }
            if (context.connection == nullptr || context.pending_input == nullptr) {
                return reject(500, 2001, "服务器内部错误");
            }

            auto session = std::make_shared<ws::WebSocketSession>(context.connection, config);
            std::weak_ptr<ws::WebSocketSession> weak_session = session;
            const service::RealtimeRegistry::MessageCallback push =
                [weak_session](const db::MessageRecord& message) {
                    if (std::shared_ptr<ws::WebSocketSession> alive = weak_session.lock()) {
                        try {
                            alive->postText(
                                json{{"type", "message"},
                                     {"data", detail::messageJson(message)}}.dump());
                        } catch (const std::exception& e) {
                            LOG_ERROR("编码实时消息失败：%s", e.what());
                        }
                    }
                };

            std::uint64_t subscription_id = 0;
            std::uint64_t server_max_id = 0;
            ServiceError open_error;
            if (!chat.openRealtime(auth.user.id, group_id, push, &subscription_id,
                                   &server_max_id, &open_error)) {
                return reject(open_error.http_status, open_error.code, open_error.message);
            }

            session->setCloseCallback(
                [&chat, subscription_id](std::uint16_t, const std::string&) {
                    chat.removeRealtime(subscription_id);
                });
            session->setOpenCallback([session, group_id, server_max_id]() {
                sendJson(session, json{{"type", "ready"},
                                       {"group_id", group_id},
                                       {"server_max_id", server_max_id}});
            });
            session->setTextCallback(
                [weak_session, &chat, user_id = auth.user.id, group_id](const std::string& text) {
                    if (std::shared_ptr<ws::WebSocketSession> alive = weak_session.lock()) {
                        handleClientText(alive, chat, user_id, group_id, text);
                    }
                });

            context.connection->setMessageCallback(
                [session](const net::TcpConnectionPtr&, std::string& input) {
                    session->onData(input);
                });
            context.connection->addCloseCallback(
                [session](int) { session->onClose(); });

            context.connection->send(ws::handshakeResponse(accept));
            if (context.connection->closed()) {
                session->onClose();
                http::UpgradeDecision decision;
                decision.switched = true;
                return decision;
            }
            context.connection->setIdleTimeoutMs(0);
            session->start(*context.pending_input);
            http::UpgradeDecision decision;
            decision.switched = true;
            return decision;
        });
}

}  // namespace api
}  // namespace cofinance
