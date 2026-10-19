#pragma once

#include <sys/epoll.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cofinance/net/Epoller.h"
#include "cofinance/net/TimerQueue.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace net {

// 一个 EventLoop 绑定一个线程。fd 的事件回调、定时器回调和任务队列都在该线程执行。
// 其他线程不能直接操作连接，只能 runInLoop() 投递任务。
class EventLoop : private NonCopyable {
public:
    using EventHandler = std::function<void(std::uint32_t)>;
    using Task = std::function<void()>;

    EventLoop() = default;
    ~EventLoop();

    bool init(std::string* err);

    bool addFd(int fd, std::uint32_t events, EventHandler handler, std::string* err);
    bool modFd(int fd, std::uint32_t events, std::string* err);
    bool removeFd(int fd);

    TimerQueue::TimerId addTimer(std::int64_t delay_ms, TimerQueue::Callback cb);
    bool cancelTimer(TimerQueue::TimerId id);

    void runInLoop(Task task);
    void run();
    void stop();
    Epoller::WaitResult pollOnce(int timeout_ms);

    bool isLoopThread() const;
    bool isStopping() const noexcept { return quit_.load(std::memory_order_acquire); }

private:
    void wakeup();
    void handleWakeup();
    void doPendingTasks();

    Epoller epoller_;
    TimerQueue timer_queue_;
    int wakeup_fd_ = -1;
    std::unordered_map<int, EventHandler> handlers_;

    std::mutex tasks_mutex_;
    std::vector<Task> pending_tasks_;
    std::atomic<bool> quit_{false};
    std::thread::id loop_thread_id_{};
};

}  // namespace net
}  // namespace cofinance
