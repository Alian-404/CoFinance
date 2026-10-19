#include "cofinance/net/TimerQueue.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace net {

TimerQueue::~TimerQueue() {
    if (timer_fd_ >= 0) {
        ::close(timer_fd_);
    }
}

bool TimerQueue::init(std::string* err) {
    if (timer_fd_ >= 0) {
        if (err != nullptr) {
            *err = "TimerQueue 已初始化";
        }
        return false;
    }
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        if (err != nullptr) {
            *err = std::string("timerfd_create 失败: ") + std::strerror(errno);
        }
        return false;
    }
    return true;
}

TimerQueue::TimerId TimerQueue::add(std::int64_t delay_ms, Callback cb) {
    const TimerId id = next_id_++;
    const std::int64_t deadline = util::nowMonotonicMillis() + (delay_ms > 0 ? delay_ms : 0);
    const auto it = timers_.emplace(deadline, Timer{deadline, id, std::move(cb)});
    index_.emplace(id, it);
    arm();
    return id;
}

bool TimerQueue::cancel(TimerId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    timers_.erase(found->second);
    index_.erase(found);
    arm();
    return true;
}

void TimerQueue::handleReadable() {
    std::uint64_t expirations = 0;
    for (;;) {
        const ssize_t n = ::read(timer_fd_, &expirations, sizeof(expirations));
        if (n == static_cast<ssize_t>(sizeof(expirations))) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    const std::int64_t now = util::nowMonotonicMillis();
    while (!timers_.empty() && timers_.begin()->first <= now) {
        auto it = timers_.begin();
        Timer timer = std::move(it->second);
        timers_.erase(it);
        index_.erase(timer.id);
        if (timer.cb) {
            timer.cb();
        }
    }
    arm();
}

void TimerQueue::arm() {
    itimerspec spec {};
    if (!timers_.empty()) {
        const std::int64_t now = util::nowMonotonicMillis();
        const std::int64_t delay = timers_.begin()->first - now;
        const std::int64_t safe_delay = delay > 0 ? delay : 1;
        spec.it_value.tv_sec = safe_delay / 1000;
        spec.it_value.tv_nsec = (safe_delay % 1000) * 1000 * 1000;
    }
    if (::timerfd_settime(timer_fd_, 0, &spec, nullptr) != 0) {
        LOG_ERROR("timerfd_settime 失败: %s", std::strerror(errno));
    }
}

}  // namespace net
}  // namespace cofinance
