#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class UserService;
class ChatService;
}
namespace api {

class ChatApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::ChatService& chat);
};

}  // namespace api
}  // namespace cofinance
