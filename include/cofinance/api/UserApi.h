#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class UserService;
}
namespace api {

class UserApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users);
};

}  // namespace api
}  // namespace cofinance
