#include "cofinance/service/ChatService.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/GroupDao.h"
#include "cofinance/db/TransactionGuard.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {
namespace {

constexpr std::uint64_t kMaxMessageLength = 2000;
constexpr std::uint64_t kMaxPageLimit = 200;
constexpr std::uint64_t kMaxPollLimit = 200;

void setError(ServiceError* error, int status, int code, std::string message) {
    if (error != nullptr) {
        error->http_status = status;
        error->code = code;
        error->message = std::move(message);
    }
}

void setDatabaseError(ServiceError* error, const char* operation, const std::string& detail) {
    LOG_ERROR("%s 失败：%s", operation, detail.c_str());
    setError(error, 500, 2001, "服务器内部错误");
}

bool validUuid(const std::string& value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
            continue;
        }
        const char c = value[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

bool normalizeContent(const std::string& raw, std::string* content, ServiceError* error) {
    *content = util::trim(raw);
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(*content, &valid_utf8);
    if (!valid_utf8 || length == 0 || length > kMaxMessageLength) {
        setError(error, 400, 1001, "消息不能为空，且最多 2000 个字符");
        return false;
    }
    for (char c : *content) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc < 0x20 && c != '\n' && c != '\r' && c != '\t') || uc == 0x7f) {
            setError(error, 400, 1001, "消息不能包含控制字符");
            return false;
        }
    }
    return true;
}

bool requireActiveGroupAndMember(db::GroupDao& groups, std::uint64_t group_id,
                                 std::uint64_t user_id, ServiceError* error,
                                 std::string* db_error) {
    std::optional<db::GroupRecord> group;
    if (!groups.findGroup(group_id, &group, db_error)) {
        setDatabaseError(error, "读取财务组", *db_error);
        return false;
    }
    if (!group.has_value() || group->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!groups.findMembership(group_id, user_id, &member, db_error)) {
        setDatabaseError(error, "读取组成员", *db_error);
        return false;
    }
    if (!member.has_value() || member->status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool lockActiveGroupAndMember(db::GroupDao& groups, std::uint64_t group_id,
                              std::uint64_t user_id, ServiceError* error,
                              std::string* db_error) {
    std::optional<db::GroupRecord> group;
    if (!groups.lockGroup(group_id, &group, db_error)) {
        setDatabaseError(error, "锁定财务组", *db_error);
        return false;
    }
    if (!group.has_value() || group->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!groups.lockMembership(group_id, user_id, &member, db_error)) {
        setDatabaseError(error, "锁定组成员", *db_error);
        return false;
    }
    if (!member.has_value() || member->status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool loadReadState(db::MessageDao& messages, std::uint64_t group_id, std::uint64_t user_id,
                   UnreadState* state, ServiceError* error, std::string* db_error) {
    if (!messages.readWatermark(group_id, user_id, &state->last_read_message_id, db_error)) {
        setDatabaseError(error, "读取已读水位线", *db_error);
        return false;
    }
    if (!messages.maxId(group_id, &state->max_message_id, db_error) ||
        !messages.countAfter(group_id, state->last_read_message_id, &state->unread_count,
                             db_error)) {
        setDatabaseError(error, "统计未读消息", *db_error);
        return false;
    }
    return true;
}

}  // namespace

ChatService::ChatService(db::ConnectionPool& pool, LongPollLimits poll_limits)
    : pool_(pool), poll_registry_(poll_limits) {}

bool ChatService::sendMessage(std::uint64_t user_id, std::uint64_t group_id,
                              const std::string& content,
                              const std::optional<std::string>& client_msg_id,
                              db::MessageRecord* message, ServiceError* error) {
    if (message == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string normalized_content;
    if (!normalizeContent(content, &normalized_content, error)) {
        return false;
    }
    std::optional<std::string> normalized_client_id = client_msg_id;
    if (normalized_client_id.has_value()) {
        if (!validUuid(*normalized_client_id)) {
            setError(error, 400, 1001, "client_msg_id 必须是 UUID 格式");
            return false;
        }
        *normalized_client_id = util::toLower(*normalized_client_id);
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启发送消息事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    if (!lockActiveGroupAndMember(groups, group_id, user_id, error, &db_error)) {
        return false;
    }
    db::MessageDao messages(lease.connection());
    if (normalized_client_id.has_value()) {
        std::optional<db::MessageRecord> existing;
        if (!messages.findBySenderClient(user_id, *normalized_client_id, &existing, &db_error)) {
            setDatabaseError(error, "查询幂等消息", db_error);
            return false;
        }
        if (existing.has_value()) {
            if (existing->group_id != group_id) {
                setError(error, 409, 1018, "client_msg_id 已在其他组使用");
                return false;
            }
            *message = std::move(*existing);
            if (!transaction.commit(&db_error)) {
                setDatabaseError(error, "提交幂等消息事务", db_error);
                return false;
            }
            return true;
        }
    }

    db::MessageRecord created;
    created.group_id = group_id;
    created.sender_id = user_id;
    created.type = 0;
    created.content = std::move(normalized_content);
    created.client_msg_id = std::move(normalized_client_id);
    bool duplicate = false;
    if (!messages.insert(created, &db_error, &duplicate)) {
        if (duplicate && created.client_msg_id.has_value()) {
            transaction.rollback();
            std::optional<db::MessageRecord> existing;
            if (!messages.findBySenderClient(user_id, *created.client_msg_id, &existing,
                                             &db_error)) {
                setDatabaseError(error, "回查并发幂等消息", db_error);
                return false;
            }
            if (!existing.has_value()) {
                setDatabaseError(error, "回查并发幂等消息", "唯一键冲突后找不到首条消息");
                return false;
            }
            if (existing->group_id != group_id) {
                setError(error, 409, 1018, "client_msg_id 已在其他组使用");
                return false;
            }
            *message = std::move(*existing);
            return true;
        }
        setDatabaseError(error, "写入消息", db_error);
        return false;
    }
    std::optional<db::MessageRecord> inserted;
    if (!messages.findById(created.id, &inserted, &db_error) || !inserted.has_value()) {
        setDatabaseError(error, "读取新消息", db_error.empty() ? "消息写入后不可见" : db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交发送消息事务", db_error);
        return false;
    }
    *message = std::move(*inserted);
    {
        std::lock_guard<std::mutex> lock(poll_cache_mutex_);
        ++group_versions_[group_id];
    }
    notifyGroup(group_id);
    return true;
}

bool ChatService::listMessages(std::uint64_t user_id, std::uint64_t group_id,
                               const MessageListInput& input, MessagePage* page,
                               ServiceError* error) {
    if (page == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (input.cursor.has_value() && input.after_id.has_value()) {
        setError(error, 400, 1001, "cursor 与 after_id 不能同时使用");
        return false;
    }
    if (input.limit == 0 || input.limit > kMaxPageLimit) {
        setError(error, 400, 1001, "limit 必须在 1-200 之间");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    if (!requireActiveGroupAndMember(groups, group_id, user_id, error, &db_error)) {
        return false;
    }
    db::MessageDao messages(lease.connection());
    db::MessageFilter filter;
    filter.cursor = input.cursor;
    filter.after_id = input.after_id;
    filter.limit = input.limit;
    bool has_more = false;
    page->items.clear();
    page->next_cursor.reset();
    if (!messages.list(group_id, filter, &page->items, &has_more, &db_error)) {
        setDatabaseError(error, "读取消息列表", db_error);
        return false;
    }
    if (has_more && !page->items.empty()) {
        page->next_cursor = page->items.back().id;
    }
    if (!messages.maxId(group_id, &page->server_max_id, &db_error)) {
        setDatabaseError(error, "读取消息最大 id", db_error);
        return false;
    }
    return true;
}

bool ChatService::pollSnapshot(std::uint64_t user_id, std::uint64_t group_id,
                               const std::optional<std::uint64_t>& after_id, std::uint64_t limit,
                               PollSnapshot* snapshot, ServiceError* error) {
    if (snapshot == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (limit == 0 || limit > kMaxPollLimit) {
        setError(error, 400, 1001, "limit 必须在 1-200 之间");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    if (!requireActiveGroupAndMember(groups, group_id, user_id, error, &db_error)) {
        return false;
    }
    db::MessageDao messages(lease.connection());
    snapshot->items.clear();
    if (!messages.maxId(group_id, &snapshot->server_max_id, &db_error)) {
        setDatabaseError(error, "读取消息最大 id", db_error);
        return false;
    }
    if (!after_id.has_value()) {
        // 缺省游标表示“只看此刻之后的新消息”，不是从 0 回放全部历史。
        snapshot->effective_after_id = snapshot->server_max_id;
        return true;
    }

    snapshot->effective_after_id = *after_id;
    db::MessageFilter filter;
    filter.after_id = after_id;
    filter.limit = limit;
    bool has_more = false;
    if (!messages.list(group_id, filter, &snapshot->items, &has_more, &db_error)) {
        setDatabaseError(error, "读取增量消息", db_error);
        return false;
    }
    if (!messages.maxId(group_id, &snapshot->server_max_id, &db_error)) {
        setDatabaseError(error, "读取消息最大 id", db_error);
        return false;
    }
    return true;
}

bool ChatService::markRead(std::uint64_t user_id, std::uint64_t group_id,
                           std::uint64_t message_id, UnreadState* state,
                           ServiceError* error) {
    if (state == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    if (!requireActiveGroupAndMember(groups, group_id, user_id, error, &db_error)) {
        return false;
    }
    db::MessageDao messages(lease.connection());
    std::uint64_t max_id = 0;
    if (!messages.maxId(group_id, &max_id, &db_error)) {
        setDatabaseError(error, "读取消息最大 id", db_error);
        return false;
    }
    const std::uint64_t clamped = message_id > max_id ? max_id : message_id;
    if (!messages.updateReadWatermark(group_id, user_id, clamped, &db_error)) {
        setDatabaseError(error, "更新已读水位线", db_error);
        return false;
    }
    if (!loadReadState(messages, group_id, user_id, state, error, &db_error)) {
        return false;
    }
    return true;
}

bool ChatService::getUnread(std::uint64_t user_id, std::uint64_t group_id,
                            UnreadState* state, ServiceError* error) {
    if (state == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    if (!requireActiveGroupAndMember(groups, group_id, user_id, error, &db_error)) {
        return false;
    }
    db::MessageDao messages(lease.connection());
    return loadReadState(messages, group_id, user_id, state, error, &db_error);
}

bool ChatService::registerWaiter(std::uint64_t user_id, std::uint64_t group_id,
                                 std::uint64_t after_id,
                                 std::function<void()> wake, std::uint64_t* waiter_id,
                                 ServiceError* error) {
    const LongPollRegisterResult result =
        poll_registry_.add(group_id, user_id, after_id, std::move(wake), waiter_id);
    if (result == LongPollRegisterResult::Ok) {
        return true;
    }
    setError(error, 429, 3002, "长轮询挂起连接数超限");
    return false;
}

void ChatService::removeWaiter(std::uint64_t waiter_id) {
    poll_registry_.remove(waiter_id);
}

void ChatService::notifyGroup(std::uint64_t group_id) {
    warmPollCache(group_id);
    poll_registry_.notify(group_id);
}

std::uint64_t ChatService::groupVersion(std::uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(poll_cache_mutex_);
    const auto found = group_versions_.find(group_id);
    return found == group_versions_.end() ? 0 : found->second;
}

void ChatService::warmPollCache(std::uint64_t group_id) {
    const std::vector<LongPollRegistry::WaiterInfo> waiters =
        poll_registry_.waitersForGroup(group_id);
    if (waiters.empty()) {
        return;
    }

    std::uint64_t min_after = std::numeric_limits<std::uint64_t>::max();
    for (const LongPollRegistry::WaiterInfo& waiter : waiters) {
        if (waiter.after_id < min_after) {
            min_after = waiter.after_id;
        }
    }

    PollCache cache;
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        LOG_WARN("预热长轮询缓存失败，将回退逐请求查询：%s", db_error.c_str());
        return;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!groups.findGroup(group_id, &group, &db_error)) {
        LOG_WARN("预热长轮询缓存读取组失败，将回退逐请求查询：%s", db_error.c_str());
        return;
    }
    if (!group.has_value() || group->status != 0) {
        cache.valid = true;
        cache.group_active = false;
        cache.expires_at_ms = util::nowMonotonicMillis() + 5000;
        std::lock_guard<std::mutex> lock(poll_cache_mutex_);
        poll_cache_[group_id] = std::move(cache);
        return;
    }

    std::vector<db::MembershipRecord> members;
    if (!groups.listJoinedMembers(group_id, &members, &db_error)) {
        LOG_WARN("预热长轮询缓存读取成员失败，将回退逐请求查询：%s", db_error.c_str());
        return;
    }
    db::MessageDao messages(lease.connection());
    db::MessageFilter filter;
    filter.after_id = min_after;
    filter.limit = 200;
    bool has_more = false;
    if (!messages.list(group_id, filter, &cache.messages, &has_more, &db_error) ||
        !messages.maxId(group_id, &cache.server_max_id, &db_error)) {
        LOG_WARN("预热长轮询缓存读取消息失败，将回退逐请求查询：%s", db_error.c_str());
        return;
    }
    cache.active_users.reserve(members.size());
    for (const db::MembershipRecord& member : members) {
        cache.active_users.insert(member.user_id);
    }
    cache.valid = true;
    cache.group_active = true;
    cache.expires_at_ms = util::nowMonotonicMillis() + 5000;
    std::lock_guard<std::mutex> lock(poll_cache_mutex_);
    poll_cache_[group_id] = std::move(cache);
}

bool ChatService::pollSnapshotFast(std::uint64_t user_id, std::uint64_t group_id,
                                   std::uint64_t after_id, std::uint64_t limit,
                                   PollSnapshot* snapshot, bool* served,
                                   ServiceError* error) {
    *served = false;
    PollSnapshot result;
    result.effective_after_id = after_id;
    std::lock_guard<std::mutex> lock(poll_cache_mutex_);
    const auto found = poll_cache_.find(group_id);
    if (found == poll_cache_.end()) {
        return true;
    }
    PollCache& cache = found->second;
    const std::int64_t now = util::nowMonotonicMillis();
    if (!cache.valid || cache.expires_at_ms < now) {
        poll_cache_.erase(found);
        return true;
    }

    *served = true;
    if (!cache.group_active) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    if (cache.active_users.find(user_id) == cache.active_users.end()) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    result.server_max_id = cache.server_max_id;
    for (const db::MessageRecord& message : cache.messages) {
        if (message.id <= after_id) {
            continue;
        }
        result.items.push_back(message);
        if (result.items.size() >= limit) {
            break;
        }
    }
    *snapshot = std::move(result);
    return true;
}

}  // namespace service
}  // namespace cofinance
