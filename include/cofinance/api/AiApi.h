#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class AiService;
class UserService;
}
namespace api {

class AiApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::AiService& ai);
};

}  // namespace api
}  // namespace cofinance
