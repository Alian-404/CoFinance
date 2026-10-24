#pragma once

#include "cofinance/ws/WebSocketSession.h"

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class UserService;
class ChatService;
}
namespace api {

class WebSocketApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::ChatService& chat,
                               const ws::WebSocketSession::Config& config);
};

}  // namespace api
}  // namespace cofinance
