#include "cofinance/service/LongPollRegistry.h"

#include <exception>
#include <utility>
#include <vector>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {

LongPollRegistry::LongPollRegistry(LongPollLimits limits) : limits_(limits) {}

LongPollRegisterResult LongPollRegistry::add(std::uint64_t group_id, std::uint64_t user_id,
                                             std::uint64_t after_id,
                                             std::function<void()> wake,
                                             std::uint64_t* waiter_id) {
    if (waiter_id == nullptr) {
        return LongPollRegisterResult::TotalLimit;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiters_.size() >= limits_.total) {
        return LongPollRegisterResult::TotalLimit;
    }
    if (by_user_[user_id] >= limits_.per_user) {
        return LongPollRegisterResult::UserLimit;
    }
    if (by_group_[group_id].size() >= limits_.per_group) {
        return LongPollRegisterResult::GroupLimit;
    }

    const std::uint64_t id = next_id_++;
    Waiter waiter;
    waiter.id = id;
    waiter.group_id = group_id;
    waiter.user_id = user_id;
    waiter.after_id = after_id;
    waiter.wake = std::move(wake);
    waiters_.emplace(id, std::move(waiter));
    by_group_[group_id].insert(id);
    ++by_user_[user_id];
    *waiter_id = id;
    return LongPollRegisterResult::Ok;
}

void LongPollRegistry::remove(std::uint64_t waiter_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = waiters_.find(waiter_id);
    if (found == waiters_.end()) {
        return;
    }
    const Waiter waiter = std::move(found->second);
    waiters_.erase(found);

    const auto group = by_group_.find(waiter.group_id);
    if (group != by_group_.end()) {
        group->second.erase(waiter_id);
        if (group->second.empty()) {
            by_group_.erase(group);
        }
    }
    const auto user = by_user_.find(waiter.user_id);
    if (user != by_user_.end()) {
        if (user->second > 1) {
            --user->second;
        } else {
            by_user_.erase(user);
        }
    }
}

void LongPollRegistry::notify(std::uint64_t group_id) {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto group = by_group_.find(group_id);
        if (group == by_group_.end()) {
            return;
        }
        callbacks.reserve(group->second.size());
        for (std::uint64_t id : group->second) {
            const auto waiter = waiters_.find(id);
            if (waiter != waiters_.end() && waiter->second.wake) {
                callbacks.push_back(waiter->second.wake);
            }
        }
    }

    for (std::function<void()>& callback : callbacks) {
        try {
            callback();
        } catch (const std::exception& e) {
            LOG_ERROR("长轮询唤醒回调异常：%s", e.what());
        } catch (...) {
            LOG_ERROR("长轮询唤醒回调抛出未知异常");
        }
    }
}

std::size_t LongPollRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

std::size_t LongPollRegistry::countForUser(std::uint64_t user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = by_user_.find(user_id);
    return found == by_user_.end() ? 0 : found->second;
}

std::size_t LongPollRegistry::countForGroup(std::uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = by_group_.find(group_id);
    return found == by_group_.end() ? 0 : found->second.size();
}

std::vector<LongPollRegistry::WaiterInfo> LongPollRegistry::waitersForGroup(
    std::uint64_t group_id) const {
    std::vector<WaiterInfo> result;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto group = by_group_.find(group_id);
    if (group == by_group_.end()) {
        return result;
    }
    result.reserve(group->second.size());
    for (std::uint64_t id : group->second) {
        const auto waiter = waiters_.find(id);
        if (waiter != waiters_.end()) {
            result.push_back(WaiterInfo{waiter->second.id, waiter->second.user_id,
                                        waiter->second.after_id});
        }
    }
    return result;
}

}  // namespace service
}  // namespace cofinance
