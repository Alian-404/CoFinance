#include "cofinance/net/Epoller.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace cofinance {
namespace net {
namespace {

std::string epollError(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

Epoller::~Epoller() {
    if (epfd_ >= 0) {
        ::close(epfd_);
    }
}

bool Epoller::init(std::string* err) {
    if (epfd_ >= 0) {
        if (err != nullptr) {
            *err = "Epoller 已初始化";
        }
        return false;
    }
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        if (err != nullptr) {
            *err = epollError("epoll_create1 失败");
        }
        return false;
    }
    return true;
}

bool Epoller::add(int fd, std::uint32_t events, std::string* err) {
    epoll_event ev {};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (err != nullptr) {
            *err = epollError("epoll_ctl(ADD) 失败");
        }
        return false;
    }
    return true;
}

bool Epoller::mod(int fd, std::uint32_t events, std::string* err) {
    epoll_event ev {};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
        if (err != nullptr) {
            *err = epollError("epoll_ctl(MOD) 失败");
        }
        return false;
    }
    return true;
}

bool Epoller::del(int fd, std::string* err) {
    if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
        if (err != nullptr) {
            *err = epollError("epoll_ctl(DEL) 失败");
        }
        return false;
    }
    return true;
}

Epoller::WaitResult Epoller::wait(int timeout_ms, std::string* err) {
    event_count_ = ::epoll_wait(epfd_, events_.data(), kMaxEvents, timeout_ms);
    if (event_count_ >= 0) {
        return WaitResult::Ready;
    }
    if (errno == EINTR) {
        event_count_ = 0;
        return WaitResult::Interrupted;
    }
    event_count_ = 0;
    if (err != nullptr) {
        *err = epollError("epoll_wait 失败");
    }
    return WaitResult::Error;
}

}  // namespace net
}  // namespace cofinance
