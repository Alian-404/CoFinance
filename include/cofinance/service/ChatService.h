#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cofinance/db/MessageDao.h"
#include "cofinance/service/LongPollRegistry.h"
#include "cofinance/service/RealtimeRegistry.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
class ConnectionPool;
}
namespace service {

struct MessageListInput {
    std::optional<std::uint64_t> cursor;
    std::optional<std::uint64_t> after_id;
    std::uint64_t limit = 20;
};

struct MessagePage {
    std::vector<db::MessageRecord> items;
    std::optional<std::uint64_t> next_cursor;
    std::uint64_t server_max_id = 0;
};

struct PollSnapshot {
    std::vector<db::MessageRecord> items;
    std::uint64_t effective_after_id = 0;
    std::uint64_t server_max_id = 0;
};

struct UnreadState {
    std::uint64_t unread_count = 0;
    std::uint64_t last_read_message_id = 0;
    std::uint64_t max_message_id = 0;
};

class ChatService : private NonCopyable {
public:
    explicit ChatService(db::ConnectionPool& pool, LongPollLimits poll_limits = {},
                         RealtimeLimits realtime_limits = {});

    bool sendMessage(std::uint64_t user_id, std::uint64_t group_id,
                     const std::string& content,
                     const std::optional<std::string>& client_msg_id,
                     db::MessageRecord* message, ServiceError* error);
    bool listMessages(std::uint64_t user_id, std::uint64_t group_id,
                      const MessageListInput& input, MessagePage* page, ServiceError* error);
    bool pollSnapshot(std::uint64_t user_id, std::uint64_t group_id,
                      const std::optional<std::uint64_t>& after_id, std::uint64_t limit,
                      PollSnapshot* snapshot, ServiceError* error);
    bool pollSnapshotFast(std::uint64_t user_id, std::uint64_t group_id,
                          std::uint64_t after_id, std::uint64_t limit,
                          PollSnapshot* snapshot, bool* served, ServiceError* error);
    bool markRead(std::uint64_t user_id, std::uint64_t group_id, std::uint64_t message_id,
                  UnreadState* state, ServiceError* error);
    bool getUnread(std::uint64_t user_id, std::uint64_t group_id, UnreadState* state,
                   ServiceError* error);

    bool registerWaiter(std::uint64_t user_id, std::uint64_t group_id,
                        std::uint64_t after_id,
                        std::function<void()> wake, std::uint64_t* waiter_id,
                        ServiceError* error);
    void removeWaiter(std::uint64_t waiter_id);
    void notifyGroup(std::uint64_t group_id);
    std::uint64_t groupVersion(std::uint64_t group_id) const;
    std::size_t pendingPolls() const { return poll_registry_.size(); }

    bool openRealtime(std::uint64_t user_id, std::uint64_t group_id,
                      RealtimeRegistry::MessageCallback callback,
                      std::uint64_t* subscription_id, std::uint64_t* server_max_id,
                      ServiceError* error);
    void removeRealtime(std::uint64_t subscription_id);
    void removeRealtimeForUser(std::uint64_t group_id, std::uint64_t user_id);
    void removeRealtimeForGroup(std::uint64_t group_id);
    std::size_t realtimeConnections() const { return realtime_registry_.size(); }

private:
    struct PollCache {
        bool valid = false;
        bool group_active = false;
        std::int64_t expires_at_ms = 0;
        std::uint64_t server_max_id = 0;
        std::vector<db::MessageRecord> messages;
        std::unordered_set<std::uint64_t> active_users;
    };

    void warmPollCache(std::uint64_t group_id);
    db::ConnectionPool& pool_;
    LongPollRegistry poll_registry_;
    RealtimeRegistry realtime_registry_;
    mutable std::mutex poll_cache_mutex_;
    std::unordered_map<std::uint64_t, PollCache> poll_cache_;
    std::unordered_map<std::uint64_t, std::uint64_t> group_versions_;
};

}  // namespace service
}  // namespace cofinance
