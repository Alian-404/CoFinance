#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class UserService;
class GroupService;
}
namespace api {

class GroupApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::GroupService& groups);
};

}  // namespace api
}  // namespace cofinance
