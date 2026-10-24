#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class SettlementService;
class UserService;
}
namespace api {

class StatsApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::SettlementService& settlements);
};

}  // namespace api
}  // namespace cofinance
