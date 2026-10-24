#include "cofinance/net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace net {

EventLoop::~EventLoop() {
    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
    }
}

bool EventLoop::init(std::string* err) {
    if (!epoller_.init(err)) {
        return false;
    }
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        if (err != nullptr) {
            *err = std::string("eventfd 创建失败: ") + std::strerror(errno);
        }
        return false;
    }
    if (!timer_queue_.init(err)) {
        return false;
    }
    if (!addFd(wakeup_fd_, EPOLLIN | EPOLLET,
               [this](std::uint32_t) { handleWakeup(); }, err)) {
        return false;
    }
    if (!addFd(timer_queue_.fd(), EPOLLIN | EPOLLET,
               [this](std::uint32_t) { timer_queue_.handleReadable(); }, err)) {
        return false;
    }
    return true;
}

bool EventLoop::addFd(int fd, std::uint32_t events, EventHandler handler, std::string* err) {
    if (handlers_.find(fd) != handlers_.end()) {
        if (err != nullptr) {
            *err = "fd 已注册到 EventLoop";
        }
        return false;
    }
    if (!epoller_.add(fd, events, err)) {
        return false;
    }
    handlers_.emplace(fd, std::move(handler));
    return true;
}

bool EventLoop::modFd(int fd, std::uint32_t events, std::string* err) {
    return epoller_.mod(fd, events, err);
}

bool EventLoop::removeFd(int fd) {
    handlers_.erase(fd);
    std::string err;
    if (!epoller_.del(fd, &err)) {
        LOG_WARN("移除 epoll fd=%d 失败: %s", fd, err.c_str());
        return false;
    }
    return true;
}

TimerQueue::TimerId EventLoop::addTimer(std::int64_t delay_ms, TimerQueue::Callback cb) {
    return timer_queue_.add(delay_ms, std::move(cb));
}

bool EventLoop::cancelTimer(TimerQueue::TimerId id) {
    return timer_queue_.cancel(id);
}

void EventLoop::runInLoop(Task task) {
    bool need_wakeup = false;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        // 队列非空说明此前已经写过 eventfd；同一批任务只需唤醒一次。
        pending_tasks_.push_back(std::move(task));
        need_wakeup = pending_tasks_.size() == 1;
    }
    if (need_wakeup) {
        wakeup();
    }
}

void EventLoop::run() {
    loop_thread_id_ = std::this_thread::get_id();
    while (!quit_.load(std::memory_order_acquire)) {
        std::string err;
        const Epoller::WaitResult result = pollOnce(-1);
        if (result == Epoller::WaitResult::Error) {
            LOG_ERROR("事件循环退出: %s", err.c_str());
            break;
        }
    }
}

void EventLoop::stop() {
    quit_.store(true, std::memory_order_release);
    wakeup();
}

Epoller::WaitResult EventLoop::pollOnce(int timeout_ms) {
    std::string err;
    const Epoller::WaitResult result = epoller_.wait(timeout_ms, &err);
    if (result != Epoller::WaitResult::Ready) {
        if (result == Epoller::WaitResult::Error && !err.empty()) {
            LOG_ERROR("%s", err.c_str());
        }
        return result;
    }

    for (int i = 0; i < epoller_.eventCount(); ++i) {
        const epoll_event& event = epoller_.events()[i];
        const auto found = handlers_.find(event.data.fd);
        if (found == handlers_.end()) {
            continue;
        }
        // 复制回调再执行：回调可能关闭连接并从 handlers_ 中删除自己。
        EventHandler handler = found->second;
        handler(event.events);
    }
    return Epoller::WaitResult::Ready;
}

bool EventLoop::isLoopThread() const {
    return std::this_thread::get_id() == loop_thread_id_;
}

void EventLoop::wakeup() {
    const std::uint64_t one = 1;
    const ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
        LOG_ERROR("唤醒 eventfd 失败: %s", std::strerror(errno));
    }
}

void EventLoop::handleWakeup() {
    std::uint64_t value = 0;
    for (;;) {
        const ssize_t n = ::read(wakeup_fd_, &value, sizeof(value));
        if (n == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    doPendingTasks();
}

void EventLoop::doPendingTasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks.swap(pending_tasks_);
    }
    for (Task& task : tasks) {
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("事件循环任务异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("事件循环任务抛出未知异常");
        }
    }
}

}  // namespace net
}  // namespace cofinance
