#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

// 文件描述符的 RAII 包装。Socket 只负责生命周期和 TCP 监听，不承载连接状态。
class Socket : private NonCopyable {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    bool valid() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    int release() noexcept;
    void reset(int fd = -1) noexcept;

    std::uint16_t localPort() const noexcept;

    // 创建非阻塞、CLOEXEC 的 TCP 监听 socket；port=0 时由内核选择端口。
    static Socket listenTcp(const std::string& host, std::uint16_t port, int backlog,
                            std::string* err);

private:
    int fd_ = -1;
};

// accept4 同时设置 NONBLOCK 和 CLOEXEC，避免 accept 后再做两次 fcntl 的竞态。
int acceptNonBlocking(int listen_fd, sockaddr_storage* addr, socklen_t* addr_len,
                      std::string* err);

std::string formatPeer(const sockaddr_storage& addr);

}  // namespace net
}  // namespace cofinance
