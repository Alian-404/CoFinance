#include "cofinance/net/Socket.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace cofinance {
namespace net {
namespace {

std::string systemError(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset(other.fd_);
        other.fd_ = -1;
    }
    return *this;
}

int Socket::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void Socket::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

std::uint16_t Socket::localPort() const noexcept {
    if (fd_ < 0) {
        return 0;
    }
    sockaddr_storage addr {};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return 0;
}

Socket Socket::listenTcp(const std::string& host, std::uint16_t port, int backlog,
                         std::string* err) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                                 &result);
    if (rc != 0) {
        if (err != nullptr) {
            *err = std::string("解析监听地址失败: ") + ::gai_strerror(rc);
        }
        return Socket();
    }

    int last_errno = 0;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }

        int yes = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
            last_errno = errno;
            ::close(fd);
            continue;
        }
        if (ai->ai_family == AF_INET6) {
            const int no = 0;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        }
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            last_errno = errno;
            ::close(fd);
            continue;
        }
        if (::listen(fd, backlog) != 0) {
            last_errno = errno;
            ::close(fd);
            continue;
        }

        ::freeaddrinfo(result);
        return Socket(fd);
    }

    ::freeaddrinfo(result);
    if (err != nullptr) {
        *err = systemError("创建监听 socket 失败");
        if (last_errno != 0) {
            *err += std::string(" (") + std::strerror(last_errno) + ")";
        }
    }
    return Socket();
}

int acceptNonBlocking(int listen_fd, sockaddr_storage* addr, socklen_t* addr_len,
                      std::string* err) {
    const int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(addr), addr_len,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0 && err != nullptr && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        *err = systemError("accept4 失败");
    }
    return fd;
}

std::string formatPeer(const sockaddr_storage& addr) {
    char host[NI_MAXHOST] {};
    char service[NI_MAXSERV] {};
    const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), host,
                                 sizeof(host), service, sizeof(service),
                                 NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return std::string("unknown");
    }
    if (addr.ss_family == AF_INET6) {
        return std::string("[") + host + "]:" + service;
    }
    return std::string(host) + ":" + service;
}

}  // namespace net
}  // namespace cofinance
