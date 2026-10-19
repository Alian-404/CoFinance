#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

// 单线程定时器队列：timerfd 负责把「时间到达」变成 epoll 可读事件，
// multimap 负责按 deadline 排序。所有操作必须发生在所属 EventLoop 线程。
class TimerQueue : private NonCopyable {
public:
    using Callback = std::function<void()>;
    using TimerId = std::uint64_t;

    TimerQueue() = default;
    ~TimerQueue();

    bool init(std::string* err);
    int fd() const noexcept { return timer_fd_; }

    TimerId add(std::int64_t delay_ms, Callback cb);
    bool cancel(TimerId id);
    void handleReadable();
    std::size_t size() const noexcept { return timers_.size(); }

private:
    struct Timer {
        std::int64_t deadline_ms = 0;
        TimerId id = 0;
        Callback cb;
    };
    using TimerMap = std::multimap<std::int64_t, Timer>;
    using TimerIterator = TimerMap::iterator;

    void arm();

    int timer_fd_ = -1;
    TimerId next_id_ = 1;
    TimerMap timers_;
    std::unordered_map<TimerId, TimerIterator> index_;
};

}  // namespace net
}  // namespace cofinance
