#pragma once

#include <sys/epoll.h>

#include <array>
#include <cstdint>
#include <string>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

class Epoller : private NonCopyable {
public:
    enum class WaitResult { Ready, Interrupted, Error };

    Epoller() = default;
    ~Epoller();

    bool init(std::string* err);
    bool add(int fd, std::uint32_t events, std::string* err);
    bool mod(int fd, std::uint32_t events, std::string* err);
    bool del(int fd, std::string* err);

    WaitResult wait(int timeout_ms, std::string* err);
    const epoll_event* events() const noexcept { return events_.data(); }
    int eventCount() const noexcept { return event_count_; }

private:
    static constexpr int kMaxEvents = 256;

    int epfd_ = -1;
    std::array<epoll_event, kMaxEvents> events_{};
    int event_count_ = 0;
};

}  // namespace net
}  // namespace cofinance
