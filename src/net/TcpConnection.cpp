#include "cofinance/net/TcpConnection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace net {

TcpConnection::TcpConnection(EventLoop* loop, int fd, std::string peer, int idle_timeout_ms)
    : loop_(loop), fd_(fd), peer_(std::move(peer)), idle_timeout_ms_(idle_timeout_ms) {}

TcpConnection::~TcpConnection() {
    releaseResources(false);
}

bool TcpConnection::start(std::string* err) {
    std::weak_ptr<TcpConnection> weak = weak_from_this();
    interest_ = EPOLLIN | EPOLLRDHUP | EPOLLET;
    if (!loop_->addFd(fd_, interest_,
                      [weak](std::uint32_t events) {
                          if (TcpConnectionPtr conn = weak.lock()) {
                              conn->handleEvent(events);
                          }
                      },
                      err)) {
        return false;
    }
    registered_ = true;
    refreshIdleTimer();
    return true;
}

void TcpConnection::setIdleTimeoutMs(int timeout_ms) {
    idle_timeout_ms_ = timeout_ms;
    if (closed_) {
        return;
    }
    if (idle_timer_id_ != 0) {
        loop_->cancelTimer(idle_timer_id_);
        idle_timer_id_ = 0;
    }
    if (idle_timeout_ms_ > 0) {
        refreshIdleTimer();
    }
}

void TcpConnection::send(std::string_view data) {
    if (closed_ || data.empty()) {
        return;
    }
    const std::size_t pending = output_.size() - output_offset_;
    if (pending + data.size() > kMaxPendingOutputBytes) {
        LOG_WARN("连接 %s 待发送数据超过上限，主动关闭 fd=%d", peer_.c_str(), fd_);
        close();
        return;
    }
    output_.append(data.data(), data.size());
    // 已在连接所属 IO 线程时直接尝试写出，避免响应排在此前投递的批量任务之后。
    // 跨线程调用仍只更新 EPOLLOUT，由所属 EventLoop 完成实际写操作。
    if (loop_->isLoopThread()) {
        handleWrite();
        return;
    }
    updateInterest();
}

void TcpConnection::runInLoop(std::function<void()> task) {
    loop_->runInLoop(std::move(task));
}

void TcpConnection::shutdownAfterWrite() {
    if (closed_ || close_after_write_) {
        return;
    }
    close_after_write_ = true;
    if (output_.size() == output_offset_) {
        close();
        return;
    }
    updateInterest();
}

void TcpConnection::close() {
    releaseResources(true);
}

TimerQueue::TimerId TcpConnection::addTimer(std::int64_t delay_ms,
                                            std::function<void()> cb) {
    return loop_->addTimer(delay_ms, std::move(cb));
}

bool TcpConnection::cancelTimer(TimerQueue::TimerId id) {
    return loop_->cancelTimer(id);
}

void TcpConnection::handleEvent(std::uint32_t events) {
    if (closed_) {
        return;
    }
    if ((events & EPOLLERR) != 0) {
        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &len);
        LOG_DEBUG("连接 %s 发生 EPOLLERR: %s", peer_.c_str(), std::strerror(socket_error));
        close();
        return;
    }
    if (close_after_write_) {
        if ((events & EPOLLOUT) != 0) {
            handleWrite();
        }
        return;
    }
    if ((events & (EPOLLIN | EPOLLRDHUP)) != 0) {
        handleRead();
        if (closed_) {
            return;
        }
    }
    if ((events & EPOLLOUT) != 0) {
        handleWrite();
        if (closed_) {
            return;
        }
    }
    if ((events & EPOLLHUP) != 0) {
        if (output_.size() > output_offset_) {
            peer_eof_ = true;
            updateInterest();
        } else {
            close();
        }
    }
}

void TcpConnection::handleRead() {
    bool got_data = false;
    char buffer[64 * 1024];

    // ET 模式下必须读到 EAGAIN：否则内核不会为剩余数据再次通知，连接会永久卡住。
    for (;;) {
        const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
        if (n > 0) {
            if (input_.size() + static_cast<std::size_t>(n) > max_input_bytes_) {
                LOG_WARN("连接 %s 输入缓冲超过上限，主动关闭 fd=%d", peer_.c_str(), fd_);
                close();
                return;
            }
            input_.append(buffer, static_cast<std::size_t>(n));
            got_data = true;
            continue;
        }
        if (n == 0) {
            peer_eof_ = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        LOG_DEBUG("连接 %s 读取失败: %s", peer_.c_str(), std::strerror(errno));
        close();
        return;
    }

    if (got_data) {
        refreshIdleTimer();
        // 业务回调可能把连接升级为 WebSocket 并替换 message_cb_；必须先复制，
        // 否则赋值会析构“正在执行”的 std::function，造成 use-after-free。
        MessageCallback callback = message_cb_;
        if (callback) {
            callback(shared_from_this(), input_);
        } else {
            input_.clear();
        }
        if (closed_) {
            return;
        }
        if (input_.size() > max_input_bytes_) {
            LOG_WARN("连接 %s 的业务回调未消费输入缓冲，主动关闭", peer_.c_str());
            close();
            return;
        }
    }

    if (peer_eof_) {
        updateInterest();
        if (!closed_ && output_.size() == output_offset_) {
            close();
        }
    }
}

void TcpConnection::handleWrite() {
    if (output_.size() == output_offset_) {
        if (close_after_write_) {
            close();
        }
        return;
    }

    bool sent_any = false;
    while (output_offset_ < output_.size()) {
        const ssize_t n = ::send(fd_, output_.data() + output_offset_,
                                 output_.size() - output_offset_, MSG_NOSIGNAL);
        if (n > 0) {
            output_offset_ += static_cast<std::size_t>(n);
            sent_any = true;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        LOG_DEBUG("连接 %s 写入失败: %s", peer_.c_str(), std::strerror(errno));
        close();
        return;
    }

    if (sent_any) {
        refreshIdleTimer();
    }
    if (output_offset_ == output_.size()) {
        output_.clear();
        output_offset_ = 0;
        if (close_after_write_) {
            close();
            return;
        }
        updateInterest();
        if (!closed_ && peer_eof_) {
            close();
        }
        return;
    }
    updateInterest();
}

void TcpConnection::updateInterest() {
    if (closed_ || !registered_) {
        return;
    }

    std::uint32_t events = EPOLLET | EPOLLRDHUP;
    if (!peer_eof_ && !close_after_write_) {
        events |= EPOLLIN;
    }
    if (output_.size() > output_offset_) {
        events |= EPOLLOUT;
    }
    if (events == interest_) {
        return;
    }

    std::string err;
    if (!loop_->modFd(fd_, events, &err)) {
        LOG_ERROR("更新连接 %s 的 epoll 事件失败: %s", peer_.c_str(), err.c_str());
        close();
        return;
    }
    interest_ = events;
}

void TcpConnection::refreshIdleTimer() {
    if (closed_ || idle_timeout_ms_ <= 0) {
        return;
    }
    if (idle_timer_id_ != 0) {
        loop_->cancelTimer(idle_timer_id_);
    }
    std::weak_ptr<TcpConnection> weak = weak_from_this();
    idle_timer_id_ = loop_->addTimer(idle_timeout_ms_, [weak]() {
        if (TcpConnectionPtr conn = weak.lock()) {
            LOG_DEBUG("连接 %s 空闲超时，关闭 fd=%d", conn->peer_.c_str(), conn->fd_);
            conn->close();
        }
    });
}

void TcpConnection::releaseResources(bool notify) {
    if (closed_) {
        return;
    }
    closed_ = true;

    if (idle_timer_id_ != 0) {
        loop_->cancelTimer(idle_timer_id_);
        idle_timer_id_ = 0;
    }
    if (registered_) {
        loop_->removeFd(fd_);
        registered_ = false;
    }

    const int closed_fd = fd_;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    message_cb_ = nullptr;
    std::vector<CloseCallback> callbacks;
    callbacks.reserve(close_cbs_.size() + (close_cb_ ? 1 : 0));
    if (close_cb_) {
        callbacks.push_back(std::move(close_cb_));
    }
    close_cb_ = nullptr;
    callbacks.insert(callbacks.end(), std::make_move_iterator(close_cbs_.begin()),
                     std::make_move_iterator(close_cbs_.end()));
    close_cbs_.clear();
    if (notify) {
        for (CloseCallback& callback : callbacks) {
            if (callback) {
                callback(closed_fd);
            }
        }
    }
}

}  // namespace net
}  // namespace cofinance
