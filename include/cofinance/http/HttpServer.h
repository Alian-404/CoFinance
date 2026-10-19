#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "cofinance/http/HttpTypes.h"
#include "cofinance/http/Router.h"
#include "cofinance/http/StaticFileHandler.h"
#include "cofinance/net/TcpConnection.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace http {

class HttpConnection;

struct HttpServerConfig {
    std::size_t header_limit = 8192;
    std::size_t body_limit = 1024 * 1024;
    int request_timeout_ms = 10000;
    std::string static_root = "www";
};

class HttpServer : private NonCopyable {
public:
    explicit HttpServer(HttpServerConfig cfg);

    using HealthCheck = std::function<bool(std::string* err)>;
    void setHealthCheck(HealthCheck check);
    void addRoute(std::string method, std::string path, Router::Handler handler);

    bool init(std::string* err);
    void onConnection(const net::TcpConnectionPtr& connection);

private:
    friend class HttpConnection;

    HttpResponse handleRequest(HttpRequest& request) const;
    static HttpResponse errorResponse(int status);

    HttpServerConfig cfg_;
    Router router_;
    std::unique_ptr<StaticFileHandler> static_files_;
    HealthCheck health_check_;
};

}  // namespace http
}  // namespace cofinance
