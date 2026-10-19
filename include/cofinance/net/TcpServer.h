#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cofinance/net/EventLoop.h"
#include "cofinance/net/Socket.h"
#include "cofinance/net/TcpConnection.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

class IoWorker;

struct TcpServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 9006;
    int backlog = 512;
    int io_threads = 4;
    std::size_t max_connections = 20000;
    int idle_timeout_ms = 60000;
};

class TcpServer : private NonCopyable {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;

    explicit TcpServer(const TcpServerConfig& cfg, ConnectionCallback on_connection = {});
    ~TcpServer();

    bool init(std::string* err);
    bool run(const std::function<bool()>& should_stop, std::string* err);
    void requestStop() noexcept { stopping_.store(true, std::memory_order_release); }

    std::uint16_t port() const noexcept { return listen_socket_.localPort(); }
    std::size_t activeConnections() const noexcept {
        return active_connections_.load(std::memory_order_relaxed);
    }

private:
    void handleAccept();
    void onConnectionRemoved() noexcept;
    void stopWorkers();

    TcpServerConfig cfg_;
    ConnectionCallback on_connection_;
    Socket listen_socket_;
    EventLoop main_loop_;
    std::vector<std::unique_ptr<IoWorker>> workers_;
    std::size_t next_worker_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> active_connections_{0};
    bool accept_registered_ = false;
};

}  // namespace net
}  // namespace cofinance
