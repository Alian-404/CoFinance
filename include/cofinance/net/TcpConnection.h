#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "cofinance/net/EventLoop.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// 单个连接的状态与缓冲区。所有方法只允许在所属 EventLoop 线程调用。
class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private NonCopyable {
public:
    using MessageCallback = std::function<void(const TcpConnectionPtr&, std::string&)>;
    using CloseCallback = std::function<void(int fd)>;

    TcpConnection(EventLoop* loop, int fd, std::string peer, int idle_timeout_ms);
    ~TcpConnection();

    bool start(std::string* err);
    void setMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }
    void setInputLimit(std::size_t bytes) noexcept { max_input_bytes_ = bytes; }

    void send(std::string_view data);
    void shutdownAfterWrite();
    void close();

    TimerQueue::TimerId addTimer(std::int64_t delay_ms, std::function<void()> cb);
    bool cancelTimer(TimerQueue::TimerId id);

    int fd() const noexcept { return fd_; }
    const std::string& peer() const noexcept { return peer_; }
    bool closed() const noexcept { return closed_; }

private:
    void handleEvent(std::uint32_t events);
    void handleRead();
    void handleWrite();
    void updateInterest();
    void refreshIdleTimer();
    void releaseResources(bool notify);

    static constexpr std::size_t kMaxPendingOutputBytes = 8 * 1024 * 1024;

    EventLoop* loop_;
    int fd_;
    std::string peer_;
    int idle_timeout_ms_;

    std::string input_;
    std::string output_;
    std::size_t output_offset_ = 0;
    std::size_t max_input_bytes_ = 1024 * 1024;

    MessageCallback message_cb_;
    CloseCallback close_cb_;
    TimerQueue::TimerId idle_timer_id_ = 0;
    std::uint32_t interest_ = 0;
    bool registered_ = false;
    bool closed_ = false;
    bool peer_eof_ = false;
    bool close_after_write_ = false;
};

}  // namespace net
}  // namespace cofinance
