#include "cofinance/net/TcpServer.h"

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "cofinance/net/TcpConnection.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace net {

class IoWorker : private NonCopyable {
public:
    IoWorker(std::size_t index, const TcpServerConfig& cfg, TcpServer::ConnectionCallback on_connection,
             std::function<void()> on_removed)
        : index_(index),
          idle_timeout_ms_(cfg.idle_timeout_ms),
          on_connection_(std::move(on_connection)),
          on_removed_(std::move(on_removed)) {}

    bool init(std::string* err) {
        return loop_.init(err);
    }

    bool start(std::string* err) {
        try {
            thread_ = std::thread([this]() {
                const std::string name = "cof-io-" + std::to_string(index_);
                ::pthread_setname_np(::pthread_self(), name.c_str());
                loop_.run();
            });
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("创建 IO 线程失败: ") + e.what();
            }
            return false;
        }
        return true;
    }

    void dispatch(int fd, std::string peer) {
        loop_.runInLoop([this, fd, peer = std::move(peer)]() mutable {
            addConnection(fd, std::move(peer));
        });
    }

    void requestStop() {
        loop_.runInLoop([this]() {
            shutting_down_ = true;
            auto connections = std::move(connections_);
            connections_.clear();
            for (auto& item : connections) {
                item.second->close();
            }
            loop_.stop();
        });
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void addConnection(int fd, std::string peer) {
        if (shutting_down_) {
            ::close(fd);
            onRemoved();
            return;
        }

        auto connection = std::make_shared<TcpConnection>(&loop_, fd, std::move(peer),
                                                          idle_timeout_ms_);
        if (on_connection_) {
            on_connection_(connection);
        } else {
            connection->setMessageCallback([](const TcpConnectionPtr& conn, std::string& input) {
                conn->send(input);
                input.clear();
            });
        }
        connection->setCloseCallback([this](int closed_fd) {
            connections_.erase(closed_fd);
            onRemoved();
        });

        std::string err;
        if (!connection->start(&err)) {
            LOG_WARN("worker-%zu 初始化连接 fd=%d 失败: %s", index_, fd, err.c_str());
            onRemoved();
            return;
        }
        connections_.emplace(fd, std::move(connection));
        LOG_DEBUG("worker-%zu 接管连接 fd=%d", index_, fd);
    }

    void onRemoved() {
        if (on_removed_) {
            on_removed_();
        }
    }

    std::size_t index_;
    int idle_timeout_ms_;
    TcpServer::ConnectionCallback on_connection_;
    std::function<void()> on_removed_;
    EventLoop loop_;
    std::thread thread_;
    std::unordered_map<int, TcpConnectionPtr> connections_;
    bool shutting_down_ = false;
};

TcpServer::TcpServer(const TcpServerConfig& cfg, ConnectionCallback on_connection)
    : cfg_(cfg), on_connection_(std::move(on_connection)) {}

TcpServer::~TcpServer() {
    stopWorkers();
}

bool TcpServer::init(std::string* err) {
    if (!main_loop_.init(err)) {
        return false;
    }
    listen_socket_ = Socket::listenTcp(cfg_.host, cfg_.port, cfg_.backlog, err);
    if (!listen_socket_.valid()) {
        return false;
    }

    rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
        limit.rlim_cur < static_cast<rlim_t>(cfg_.max_connections + 64)) {
        LOG_WARN("RLIMIT_NOFILE=%llu，小于 max_connections+64；高并发前请执行 ulimit -n 65535",
                 static_cast<unsigned long long>(limit.rlim_cur));
    }

    if (!main_loop_.addFd(listen_socket_.fd(), EPOLLIN | EPOLLET,
                          [this](std::uint32_t) { handleAccept(); }, err)) {
        return false;
    }
    accept_registered_ = true;
    return true;
}

bool TcpServer::run(const std::function<bool()>& should_stop, std::string* err) {
    workers_.reserve(static_cast<std::size_t>(cfg_.io_threads));
    for (int i = 0; i < cfg_.io_threads; ++i) {
        auto worker = std::make_unique<IoWorker>(static_cast<std::size_t>(i), cfg_, on_connection_,
                                                  [this]() { onConnectionRemoved(); });
        if (!worker->init(err)) {
            stopWorkers();
            return false;
        }
        workers_.push_back(std::move(worker));
    }
    for (auto& worker : workers_) {
        if (!worker->start(err)) {
            stopWorkers();
            return false;
        }
    }

    LOG_INFO("TCP 服务就绪：监听 %s:%u，IO 线程 %d，最大连接 %zu，空闲超时 %dms",
             cfg_.host.c_str(), static_cast<unsigned>(listen_socket_.localPort()), cfg_.io_threads,
             cfg_.max_connections, cfg_.idle_timeout_ms);

    int64_t last_stats_ms = util::nowUtcMillis();
    bool ok = true;
    while (!stopping_.load(std::memory_order_acquire) && !should_stop()) {
        const Epoller::WaitResult result = main_loop_.pollOnce(500);
        if (result == Epoller::WaitResult::Error) {
            if (err != nullptr) {
                *err = "主 Reactor 事件循环异常";
            }
            ok = false;
            break;
        }

        const int64_t now = util::nowUtcMillis();
        if (now - last_stats_ms >= 5000) {
            last_stats_ms = now;
            LOG_INFO("运行统计: 活跃连接 %zu, 日志已写 %llu, 丢弃 %llu", activeConnections(),
                     static_cast<unsigned long long>(Logger::instance().writtenCount()),
                     static_cast<unsigned long long>(Logger::instance().droppedCount()));
        }
    }

    if (accept_registered_) {
        main_loop_.removeFd(listen_socket_.fd());
        accept_registered_ = false;
    }
    listen_socket_.reset();
    stopWorkers();
    return ok;
}

void TcpServer::handleAccept() {
    for (;;) {
        sockaddr_storage peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        std::string accept_err;
        const int fd = acceptNonBlocking(listen_socket_.fd(), &peer_addr, &peer_len, &accept_err);
        if (fd >= 0) {
            const std::size_t total =
                active_connections_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (total > cfg_.max_connections) {
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                ::close(fd);
                LOG_WARN("连接数达到上限 %zu，拒绝新连接", cfg_.max_connections);
                continue;
            }
            if (workers_.empty()) {
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                ::close(fd);
                continue;
            }
            IoWorker& worker = *workers_[next_worker_ % workers_.size()];
            ++next_worker_;
            worker.dispatch(fd, formatPeer(peer_addr));
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        LOG_ERROR("accept 失败: %s", accept_err.c_str());
        break;
    }
}

void TcpServer::onConnectionRemoved() noexcept {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void TcpServer::stopWorkers() {
    for (auto& worker : workers_) {
        worker->requestStop();
    }
    for (auto& worker : workers_) {
        worker->join();
    }
    workers_.clear();
}

}  // namespace net
}  // namespace cofinance
