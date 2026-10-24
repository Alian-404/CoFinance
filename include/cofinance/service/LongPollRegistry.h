#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace service {

struct LongPollLimits {
    std::size_t per_user = 5;
    std::size_t per_group = 200;
    std::size_t total = 4096;
};

enum class LongPollRegisterResult { Ok, UserLimit, GroupLimit, TotalLimit };

// 只负责线程安全的挂起登记与按组唤醒，不执行 SQL、不碰连接。
// 唤醒回调只应尽快把检查任务投递回连接所属 EventLoop。
class LongPollRegistry : private NonCopyable {
public:
    struct WaiterInfo {
        std::uint64_t id = 0;
        std::uint64_t user_id = 0;
        std::uint64_t after_id = 0;
    };

    explicit LongPollRegistry(LongPollLimits limits = {});

    LongPollRegisterResult add(std::uint64_t group_id, std::uint64_t user_id,
                               std::uint64_t after_id,
                               std::function<void()> wake, std::uint64_t* waiter_id);
    void remove(std::uint64_t waiter_id);
    void notify(std::uint64_t group_id);

    std::size_t size() const;
    std::size_t countForUser(std::uint64_t user_id) const;
    std::size_t countForGroup(std::uint64_t group_id) const;
    std::vector<WaiterInfo> waitersForGroup(std::uint64_t group_id) const;

private:
    struct Waiter {
        std::uint64_t id = 0;
        std::uint64_t group_id = 0;
        std::uint64_t user_id = 0;
        std::uint64_t after_id = 0;
        std::function<void()> wake;
    };

    LongPollLimits limits_;
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Waiter> waiters_;
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> by_group_;
    std::unordered_map<std::uint64_t, std::size_t> by_user_;
};

}  // namespace service
}  // namespace cofinance
