#pragma once

namespace cofinance {
namespace http {
class HttpServer;
}
namespace service {
class UserService;
class LedgerService;
}
namespace api {

class LedgerApi {
public:
    static void registerRoutes(http::HttpServer& server, service::UserService& users,
                               service::LedgerService& ledger);
};

}  // namespace api
}  // namespace cofinance
