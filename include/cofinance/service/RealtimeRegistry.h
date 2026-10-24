#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
struct MessageRecord;
}
namespace service {

struct RealtimeLimits {
    std::size_t per_user = 5;
    std::size_t per_group = 200;
    std::size_t total = 4096;
};

enum class RealtimeRegisterResult { Ok, UserLimit, GroupLimit, TotalLimit };

// WebSocket 等实时连接的订阅表。通知回调只负责把任务投递回连接所属
// EventLoop，不能在这里做 SQL 或直接操作 socket。
class RealtimeRegistry : private NonCopyable {
public:
    using MessageCallback = std::function<void(const db::MessageRecord&)>;

    explicit RealtimeRegistry(RealtimeLimits limits = {});

    RealtimeRegisterResult add(std::uint64_t group_id, std::uint64_t user_id,
                               MessageCallback callback, std::uint64_t* subscription_id);
    void remove(std::uint64_t subscription_id);
    void removeForUser(std::uint64_t group_id, std::uint64_t user_id);
    void removeForGroup(std::uint64_t group_id);
    void notify(std::uint64_t group_id, const db::MessageRecord& message);

    std::size_t size() const;
    std::size_t countForUser(std::uint64_t user_id) const;
    std::size_t countForGroup(std::uint64_t group_id) const;

private:
    struct Subscriber {
        std::uint64_t id = 0;
        std::uint64_t group_id = 0;
        std::uint64_t user_id = 0;
        MessageCallback callback;
    };

    RealtimeLimits limits_;
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Subscriber> subscribers_;
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> by_group_;
    std::unordered_map<std::uint64_t, std::size_t> by_user_;
};

}  // namespace service
}  // namespace cofinance
