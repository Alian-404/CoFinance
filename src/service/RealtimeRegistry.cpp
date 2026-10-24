#include "cofinance/service/RealtimeRegistry.h"

#include <exception>
#include <utility>
#include <vector>

#include "cofinance/db/MessageDao.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {

RealtimeRegistry::RealtimeRegistry(RealtimeLimits limits) : limits_(limits) {}

RealtimeRegisterResult RealtimeRegistry::add(std::uint64_t group_id, std::uint64_t user_id,
                                             MessageCallback callback,
                                             std::uint64_t* subscription_id) {
    if (subscription_id == nullptr || !callback) {
        return RealtimeRegisterResult::TotalLimit;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribers_.size() >= limits_.total) {
        return RealtimeRegisterResult::TotalLimit;
    }
    const auto user_count = by_user_.find(user_id);
    if (user_count != by_user_.end() && user_count->second >= limits_.per_user) {
        return RealtimeRegisterResult::UserLimit;
    }
    const auto group = by_group_.find(group_id);
    if (group != by_group_.end() && group->second.size() >= limits_.per_group) {
        return RealtimeRegisterResult::GroupLimit;
    }

    const std::uint64_t id = next_id_++;
    Subscriber subscriber;
    subscriber.id = id;
    subscriber.group_id = group_id;
    subscriber.user_id = user_id;
    subscriber.callback = std::move(callback);
    subscribers_.emplace(id, std::move(subscriber));
    by_group_[group_id].insert(id);
    ++by_user_[user_id];
    *subscription_id = id;
    return RealtimeRegisterResult::Ok;
}

void RealtimeRegistry::remove(std::uint64_t subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = subscribers_.find(subscription_id);
    if (found == subscribers_.end()) {
        return;
    }
    const Subscriber subscriber = std::move(found->second);
    subscribers_.erase(found);

    const auto group = by_group_.find(subscriber.group_id);
    if (group != by_group_.end()) {
        group->second.erase(subscription_id);
        if (group->second.empty()) {
            by_group_.erase(group);
        }
    }
    const auto user = by_user_.find(subscriber.user_id);
    if (user != by_user_.end()) {
        if (user->second > 1) {
            --user->second;
        } else {
            by_user_.erase(user);
        }
    }
}

void RealtimeRegistry::removeForUser(std::uint64_t group_id, std::uint64_t user_id) {
    std::vector<std::uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto group = by_group_.find(group_id);
        if (group == by_group_.end()) {
            return;
        }
        for (std::uint64_t id : group->second) {
            const auto subscriber = subscribers_.find(id);
            if (subscriber != subscribers_.end() && subscriber->second.user_id == user_id) {
                ids.push_back(id);
            }
        }
    }
    for (std::uint64_t id : ids) {
        remove(id);
    }
}

void RealtimeRegistry::removeForGroup(std::uint64_t group_id) {
    std::vector<std::uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto group = by_group_.find(group_id);
        if (group != by_group_.end()) {
            ids.assign(group->second.begin(), group->second.end());
        }
    }
    for (std::uint64_t id : ids) {
        remove(id);
    }
}

void RealtimeRegistry::notify(std::uint64_t group_id, const db::MessageRecord& message) {
    std::vector<MessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto group = by_group_.find(group_id);
        if (group == by_group_.end()) {
            return;
        }
        callbacks.reserve(group->second.size());
        for (std::uint64_t id : group->second) {
            const auto subscriber = subscribers_.find(id);
            if (subscriber != subscribers_.end() && subscriber->second.callback) {
                callbacks.push_back(subscriber->second.callback);
            }
        }
    }

    for (MessageCallback& callback : callbacks) {
        try {
            callback(message);
        } catch (const std::exception& e) {
            LOG_ERROR("实时消息回调异常：%s", e.what());
        } catch (...) {
            LOG_ERROR("实时消息回调抛出未知异常");
        }
    }
}

std::size_t RealtimeRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribers_.size();
}

std::size_t RealtimeRegistry::countForUser(std::uint64_t user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = by_user_.find(user_id);
    return found == by_user_.end() ? 0 : found->second;
}

std::size_t RealtimeRegistry::countForGroup(std::uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = by_group_.find(group_id);
    return found == by_group_.end() ? 0 : found->second.size();
}

}  // namespace service
}  // namespace cofinance
