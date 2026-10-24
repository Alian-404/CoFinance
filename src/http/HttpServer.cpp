#include "cofinance/http/HttpServer.h"

#include <atomic>
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
          request_timeout_ms_(cfg.request_timeout_ms),
          max_pending_input_(cfg.header_limit + cfg.body_limit + 64 * 1024) {}

    ~HttpConnection() {
        cancelRequestTimer();
        cancelAsync();
    }

    void onMessage(std::string& input) {
        net::TcpConnectionPtr connection = connection_.lock();
        if (!connection || connection->closed() || closing_) {
            input.clear();
            return;
        }

        // 一个连接上的 HTTP 响应必须按请求顺序发送。长轮询挂起期间收到的
        // pipelined 字节先暂存，等当前响应完成后再交给解析器。
        if (awaiting_async_) {
            if (input.empty()) {
                return;
            }
            if (pending_input_.size() + input.size() > max_pending_input_) {
                LOG_WARN("异步请求挂起期间管线数据超限，关闭 fd=%d", connection->fd());
                input.clear();
                connection->close();
                return;
            }
            pending_input_.append(input);
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

            DispatchResult routed;
            try {
                routed = server_->handleRequest(request, makeAsyncContext());
            } catch (const std::exception& e) {
                LOG_ERROR("HTTP 处理异常: %s", e.what());
                routed = DispatchResult{DispatchStatus::Complete,
                                        HttpServer::errorResponse(500)};
            } catch (...) {
                LOG_ERROR("HTTP 处理未知异常");
                routed = DispatchResult{DispatchStatus::Complete,
                                        HttpServer::errorResponse(500)};
            }

            if (routed.status == DispatchStatus::Pending) {
                awaiting_async_ = true;
                async_keep_alive_ = request.keep_alive;
                async_head_only_ = request.method == "HEAD";
                if (!input.empty()) {
                    if (input.size() > max_pending_input_) {
                        input.clear();
                        closing_ = true;
                        connection->close();
                        return;
                    }
                    pending_input_ = std::move(input);
                }
                return;
            }

            bool keep_alive = request.keep_alive;
            HttpResponse response = std::move(routed.response);
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
    AsyncContext makeAsyncContext() {
        AsyncContext context;
        std::weak_ptr<HttpConnection> weak_self = weak_from_this();
        auto responded = std::make_shared<std::atomic<bool>>(false);

        context.respond = [weak_self, responded](HttpResponse response) {
            if (responded->exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            if (net::TcpConnectionPtr connection = [&]() {
                    if (auto self = weak_self.lock()) {
                        return self->connection_.lock();
                    }
                    return net::TcpConnectionPtr();
                }()) {
                connection->runInLoop([weak_self, response = std::move(response)]() mutable {
                    if (auto self = weak_self.lock()) {
                        self->finishAsync(std::move(response));
                    }
                });
            }
        };
        context.set_cancel = [weak_self](std::function<void()> cancel) {
            if (auto self = weak_self.lock()) {
                self->async_cancel_ = std::move(cancel);
            }
        };
        context.post = [weak_self](std::function<void()> task) {
            if (auto self = weak_self.lock()) {
                if (net::TcpConnectionPtr connection = self->connection_.lock()) {
                    connection->runInLoop(std::move(task));
                }
            }
        };
        context.add_timer = [weak_self](std::int64_t delay_ms,
                                        std::function<void()> callback) -> std::uint64_t {
            if (auto self = weak_self.lock()) {
                if (net::TcpConnectionPtr connection = self->connection_.lock()) {
                    return connection->addTimer(delay_ms, std::move(callback));
                }
            }
            return 0;
        };
        context.cancel_timer = [weak_self](std::uint64_t timer_id) {
            if (auto self = weak_self.lock()) {
                if (net::TcpConnectionPtr connection = self->connection_.lock()) {
                    return connection->cancelTimer(timer_id);
                }
            }
            return false;
        };
        return context;
    }

    void finishAsync(HttpResponse response) {
        if (!awaiting_async_) {
            return;
        }
        awaiting_async_ = false;
        cancelAsync();

        net::TcpConnectionPtr connection = connection_.lock();
        if (!connection || connection->closed()) {
            return;
        }

        bool keep_alive = async_keep_alive_;
        if (response.close) {
            keep_alive = false;
        }
        LOG_DEBUG("HTTP 异步响应 -> %d, keep_alive=%s", response.status,
                  keep_alive ? "yes" : "no");
        connection->send(response.serialize(keep_alive, async_head_only_));
        if (!keep_alive || connection->closed()) {
            closing_ = true;
            pending_input_.clear();
            connection->shutdownAfterWrite();
            return;
        }

        parser_.reset();
        if (!pending_input_.empty()) {
            std::string pending = std::move(pending_input_);
            pending_input_.clear();
            onMessage(pending);
        }
    }

    void cancelAsync() {
        if (!async_cancel_) {
            return;
        }
        std::function<void()> cancel = std::move(async_cancel_);
        async_cancel_ = nullptr;
        cancel();
    }

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
    std::size_t max_pending_input_;
    net::TimerQueue::TimerId timer_id_ = 0;
    bool request_started_ = false;
    bool closing_ = false;
    bool awaiting_async_ = false;
    bool async_keep_alive_ = true;
    bool async_head_only_ = false;
    std::string pending_input_;
    std::function<void()> async_cancel_;
};

HttpServer::HttpServer(HttpServerConfig cfg) : cfg_(std::move(cfg)) {}

void HttpServer::setHealthCheck(HealthCheck check) {
    health_check_ = std::move(check);
}

void HttpServer::addRoute(std::string method, std::string path, Router::Handler handler) {
    router_.add(std::move(method), std::move(path), std::move(handler));
}

void HttpServer::addAsyncRoute(std::string method, std::string path,
                               Router::AsyncHandler handler) {
    router_.addAsync(std::move(method), std::move(path), std::move(handler));
}

bool HttpServer::init(std::string* err) {
    static_files_ = std::make_unique<StaticFileHandler>(cfg_.static_root);
    if (!static_files_->init(err)) {
        return false;
    }

    router_.add("GET", "/api/ping", [](const HttpRequest&) {
        return HttpResponse::json(200, R"({"code":0,"message":"pong"})");
    });
    router_.add("GET", "/api/health", [this](const HttpRequest&) {
        if (!health_check_) {
            return HttpResponse::json(200, R"({"code":0,"status":"ok"})");
        }
        std::string check_error;
        if (health_check_(&check_error)) {
            return HttpResponse::json(200, R"({"code":0,"status":"ok","db":"up"})");
        }
        LOG_WARN("健康检查失败：%s", check_error.c_str());
        return HttpResponse::json(503, R"({"code":503,"status":"degraded","db":"down"})");
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

DispatchResult HttpServer::handleRequest(HttpRequest& request,
                                         const AsyncContext& context) const {
    DispatchResult routed = router_.dispatch(request, context);
    if (routed.status != DispatchStatus::NotFound) {
        return routed;
    }
    if (request.path.rfind("/api/", 0) == 0) {
        return DispatchResult{DispatchStatus::Complete, errorResponse(404)};
    }
    if (request.method == "GET" || request.method == "HEAD") {
        return DispatchResult{DispatchStatus::Complete, static_files_->handle(request)};
    }
    return DispatchResult{DispatchStatus::Complete, errorResponse(404)};
}

HttpResponse HttpServer::errorResponse(int status) {
    return HttpResponse::text(status, std::string(reasonPhrase(status)) + "\n");
}

}  // namespace http
}  // namespace cofinance
