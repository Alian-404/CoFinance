#include "cofinance/http/HttpServer.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "cofinance/http/HttpParser.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection>, private NonCopyable {
public:
    HttpConnection(std::weak_ptr<net::TcpConnection> connection, HttpServer* server,
                   const HttpServerConfig& cfg)
        : connection_(std::move(connection)),
          server_(server),
          parser_(cfg.header_limit, cfg.body_limit),
          request_timeout_ms_(cfg.request_timeout_ms) {}

    ~HttpConnection() {
        cancelRequestTimer();
    }

    void onMessage(std::string& input) {
        net::TcpConnectionPtr connection = connection_.lock();
        if (!connection || connection->closed() || closing_) {
            input.clear();
            return;
        }

        for (;;) {
            if (!request_started_) {
                request_started_ = true;
                startRequestTimer(connection);
            }
            if (input.empty()) {
                return;
            }

            HttpRequest request;
            HttpParseError parse_error;
            const HttpParser::Result result = parser_.parse(input, &request, &parse_error);
            if (result == HttpParser::Result::NeedMore) {
                return;
            }
            cancelRequestTimer();
            request_started_ = false;

            if (result == HttpParser::Result::Error) {
                LOG_DEBUG("HTTP 解析失败: status=%d, %s", parse_error.status,
                          parse_error.message.c_str());
                sendError(connection, parse_error.status);
                input.clear();
                return;
            }

            bool keep_alive = request.keep_alive;
            HttpResponse response;
            try {
                response = server_->handleRequest(request);
            } catch (const std::exception& e) {
                LOG_ERROR("HTTP 处理异常: %s", e.what());
                response = HttpServer::errorResponse(500);
            } catch (...) {
                LOG_ERROR("HTTP 处理未知异常");
                response = HttpServer::errorResponse(500);
            }
            if (response.close) {
                keep_alive = false;
            }

            LOG_DEBUG("HTTP %s %s -> %d, keep_alive=%s", request.method.c_str(),
                      request.path.c_str(), response.status, keep_alive ? "yes" : "no");
            connection->send(response.serialize(keep_alive, request.method == "HEAD"));
            if (!keep_alive || connection->closed()) {
                closing_ = true;
                input.clear();
                connection->shutdownAfterWrite();
                return;
            }

            parser_.reset();
            if (input.empty()) {
                return;
            }
        }
    }

private:
    void startRequestTimer(const net::TcpConnectionPtr& connection) {
        if (request_timeout_ms_ <= 0 || timer_id_ != 0) {
            return;
        }
        std::weak_ptr<HttpConnection> weak = weak_from_this();
        timer_id_ = connection->addTimer(request_timeout_ms_, [weak]() {
            if (std::shared_ptr<HttpConnection> self = weak.lock()) {
                self->onRequestTimeout();
            }
        });
    }

    void cancelRequestTimer() {
        if (timer_id_ == 0) {
            return;
        }
        if (net::TcpConnectionPtr connection = connection_.lock()) {
            connection->cancelTimer(timer_id_);
        }
        timer_id_ = 0;
    }

    void onRequestTimeout() {
        timer_id_ = 0;
        if (!request_started_ || closing_) {
            return;
        }
        if (net::TcpConnectionPtr connection = connection_.lock()) {
            LOG_DEBUG("HTTP 请求超时，关闭连接 fd=%d", connection->fd());
            sendError(connection, 408);
        }
    }

    void sendError(const net::TcpConnectionPtr& connection, int status) {
        if (closing_ || connection->closed()) {
            return;
        }
        cancelRequestTimer();
        request_started_ = false;
        closing_ = true;
        HttpResponse response = HttpServer::errorResponse(status);
        connection->send(response.serialize(false, false));
        connection->shutdownAfterWrite();
    }

    std::weak_ptr<net::TcpConnection> connection_;
    HttpServer* server_;
    HttpParser parser_;
    int request_timeout_ms_;
    net::TimerQueue::TimerId timer_id_ = 0;
    bool request_started_ = false;
    bool closing_ = false;
};

HttpServer::HttpServer(HttpServerConfig cfg) : cfg_(std::move(cfg)) {}

bool HttpServer::init(std::string* err) {
    static_files_ = std::make_unique<StaticFileHandler>(cfg_.static_root);
    if (!static_files_->init(err)) {
        return false;
    }

    router_.add("GET", "/api/ping", [](const HttpRequest&) {
        return HttpResponse::json(200, R"({"code":0,"message":"pong"})");
    });
    router_.add("GET", "/api/health", [](const HttpRequest&) {
        return HttpResponse::json(200, R"({"code":0,"status":"ok"})");
    });
    return true;
}

void HttpServer::onConnection(const net::TcpConnectionPtr& connection) {
    connection->setInputLimit(cfg_.header_limit + cfg_.body_limit + 64 * 1024);
    auto context = std::make_shared<HttpConnection>(connection, this, cfg_);
    connection->setMessageCallback(
        [context](const net::TcpConnectionPtr&, std::string& input) {
            context->onMessage(input);
        });
}

HttpResponse HttpServer::handleRequest(HttpRequest& request) const {
    std::optional<HttpResponse> routed = router_.dispatch(request);
    if (routed.has_value()) {
        return std::move(*routed);
    }
    if (request.path.rfind("/api/", 0) == 0) {
        return errorResponse(404);
    }
    if (request.method == "GET" || request.method == "HEAD") {
        return static_files_->handle(request);
    }
    return errorResponse(404);
}

HttpResponse HttpServer::errorResponse(int status) {
    return HttpResponse::text(status, std::string(reasonPhrase(status)) + "\n");
}

}  // namespace http
}  // namespace cofinance
