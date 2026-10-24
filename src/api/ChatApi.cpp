#include "cofinance/api/ChatApi.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/ChatService.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using json = support::json;
using http::AsyncContext;
using http::HttpRequest;
using http::HttpResponse;
using service::AuthContext;
using service::ChatService;
using service::MessageListInput;
using service::MessagePage;
using service::PollSnapshot;
using service::ServiceError;
using service::UnreadState;
using service::UserService;

json messageJson(const db::MessageRecord& message) {
    json client_msg_id = nullptr;
    if (message.client_msg_id.has_value()) {
        client_msg_id = *message.client_msg_id;
    }
    std::string sender_name = message.sender_nickname;
    if (sender_name.empty()) {
        sender_name = message.sender_username;
    }
    if (message.sender_id == 0) {
        sender_name = "系统";
    }
    return json{{"id", message.id},
                {"group_id", message.group_id},
                {"sender_id", message.sender_id},
                {"sender_username", message.sender_username},
                {"sender_nickname", message.sender_nickname},
                {"sender_avatar_url", message.sender_avatar_url},
                {"sender_name", std::move(sender_name)},
                {"type", message.type},
                {"content", message.content},
                {"client_msg_id", std::move(client_msg_id)},
                {"created_at", message.created_at}};
}

json messageListJson(const std::vector<db::MessageRecord>& records) {
    json items = json::array();
    for (const db::MessageRecord& message : records) {
        items.push_back(messageJson(message));
    }
    return items;
}

bool pathId(const HttpRequest& request, const char* name, std::uint64_t* value,
            HttpResponse* error) {
    const auto found = request.path_params.find(name);
    if (found == request.path_params.end() || !support::parseUInt64(found->second, value)) {
        *error = support::badRequest(std::string("路径参数无效: ") + name);
        return false;
    }
    return true;
}

bool parseNonNegativeUint64(const std::string& raw, std::uint64_t* value) {
    if (raw.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (char c : raw) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (parsed > (UINT64_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

bool optionalQueryId(const HttpRequest& request, const char* name,
                     std::optional<std::uint64_t>* value, HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, name, &raw, &found)) {
        *error = support::badRequest(std::string("查询参数编码无效: ") + name);
        return false;
    }
    if (!found) {
        value->reset();
        return true;
    }
    std::uint64_t parsed = 0;
    if (!parseNonNegativeUint64(raw, &parsed)) {
        *error = support::badRequest(std::string("查询参数必须是非负整数: ") + name);
        return false;
    }
    *value = parsed;
    return true;
}

bool queryLimit(const HttpRequest& request, std::uint64_t default_limit,
                std::uint64_t* limit, HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, "limit", &raw, &found)) {
        *error = support::badRequest("查询参数编码无效: limit");
        return false;
    }
    *limit = default_limit;
    if (!found) {
        return true;
    }
    if (!parseNonNegativeUint64(raw, limit) || *limit == 0) {
        *error = support::badRequest("limit 必须是正整数");
        return false;
    }
    return true;
}

bool queryTimeoutSeconds(const HttpRequest& request, int* timeout_seconds, HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, "timeout", &raw, &found)) {
        *error = support::badRequest("查询参数编码无效: timeout");
        return false;
    }
    *timeout_seconds = 30;
    if (!found) {
        return true;
    }
    std::uint64_t parsed = 0;
    if (!parseNonNegativeUint64(raw, &parsed) || parsed < 1 || parsed > 30) {
        *error = support::badRequest("timeout 必须是 1-30 秒");
        return false;
    }
    *timeout_seconds = static_cast<int>(parsed);
    return true;
}

bool optionalClientMessageId(const json& body, std::optional<std::string>* client_msg_id,
                             HttpResponse* error) {
    if (!support::optionalString(body, "client_msg_id", client_msg_id, error)) {
        return false;
    }
    if (client_msg_id->has_value() && client_msg_id->value().empty()) {
        client_msg_id->reset();
    }
    return true;
}

// 一个挂起中的 poll。所有成员方法都在连接所属 IO 线程执行；notify 只投递回来，
// 因此这里没有跨线程锁。finished 用 atomic 防御重复唤醒/超时/断开竞争。
class PollOperation : public std::enable_shared_from_this<PollOperation> {
public:
    PollOperation(ChatService& chat, AsyncContext context, std::uint64_t user_id,
                  std::uint64_t group_id, std::uint64_t after_id, std::uint64_t limit,
                  int timeout_seconds, std::uint64_t initial_version)
        : chat_(chat),
          context_(std::move(context)),
          user_id_(user_id),
          group_id_(group_id),
          after_id_(after_id),
          limit_(limit),
          timeout_seconds_(timeout_seconds),
          initial_version_(initial_version) {}

    void start() {
        const std::shared_ptr<PollOperation> self = shared_from_this();
        ServiceError error;
        if (!chat_.registerWaiter(user_id_, group_id_, after_id_,
                                  [weak = weak_from_this()]() {
                if (std::shared_ptr<PollOperation> operation = weak.lock()) {
                    operation->requestCheck();
                }
                                  }, &waiter_id_, &error)) {
            finishError(error);
            return;
        }
        // 连接析构/关闭时由 HttpConnection 调用；强引用保证 operation 活到清理完成。
        context_.set_cancel([self]() { self->cancel(); });
        timer_id_ = context_.add_timer(static_cast<std::int64_t>(timeout_seconds_) * 1000,
                                       [weak = weak_from_this()]() {
                                           if (std::shared_ptr<PollOperation> operation =
                                                   weak.lock()) {
                                               operation->check(true);
                                           }
                                       });
        if (timer_id_ == 0) {
            ServiceError timer_error;
            timer_error.http_status = 503;
            timer_error.code = 2001;
            timer_error.message = "连接正在关闭，无法挂起长轮询";
            finishError(timer_error);
            return;
        }
        // 先登记再比较组版本：只有“初始查询之后确实有新消息提交”才重新查库。
        // 否则 2000 条 poll 会在启动时各自多做一轮完整 SQL，挤占所有 IO worker。
        if (chat_.groupVersion(group_id_) != initial_version_) {
            requestCheck();
        }
    }

private:
    void requestCheck() {
        std::weak_ptr<PollOperation> weak = weak_from_this();
        context_.post([weak]() {
            if (std::shared_ptr<PollOperation> operation = weak.lock()) {
                operation->check(false);
            }
        });
    }

    void check(bool timeout) {
        if (finished_.load(std::memory_order_acquire)) {
            return;
        }
        PollSnapshot snapshot;
        ServiceError error;
        bool served = false;
        if (!chat_.pollSnapshotFast(user_id_, group_id_, after_id_, limit_, &snapshot,
                                    &served, &error)) {
            finishError(error);
            return;
        }
        if (!served &&
            !chat_.pollSnapshot(user_id_, group_id_, std::optional<std::uint64_t>(after_id_),
                                limit_, &snapshot, &error)) {
            finishError(error);
            return;
        }
        if (!snapshot.items.empty()) {
            finishSnapshot(snapshot);
            return;
        }
        if (timeout) {
            finishSnapshot(snapshot);
        }
    }

    void finishSnapshot(const PollSnapshot& snapshot) {
        if (!finishOnce()) {
            return;
        }
        context_.respond(support::jsonResult(
            200, 0, "ok",
            json{{"items", messageListJson(snapshot.items)},
                 {"server_max_id", snapshot.server_max_id}}));
    }

    void finishError(const ServiceError& error) {
        if (!finishOnce()) {
            return;
        }
        context_.respond(support::errorResult(error));
    }

    void cancel() {
        if (!finishOnce()) {
            return;
        }
    }

    bool finishOnce() {
        bool expected = false;
        if (!finished_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        if (timer_id_ != 0) {
            context_.cancel_timer(timer_id_);
            timer_id_ = 0;
        }
        if (waiter_id_ != 0) {
            chat_.removeWaiter(waiter_id_);
            waiter_id_ = 0;
        }
        return true;
    }

    ChatService& chat_;
    AsyncContext context_;
    std::uint64_t user_id_;
    std::uint64_t group_id_;
    std::uint64_t after_id_;
    std::uint64_t limit_;
    int timeout_seconds_;
    std::uint64_t initial_version_;
    std::uint64_t waiter_id_ = 0;
    std::uint64_t timer_id_ = 0;
    std::atomic<bool> finished_{false};
};

}  // namespace

void ChatApi::registerRoutes(http::HttpServer& server, UserService& users, ChatService& chat) {
    server.addRoute("GET", "/api/metrics", [&users, &chat](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!support::authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        return support::jsonResult(200, 0, "ok",
                                   json{{"pending_polls", chat.pendingPolls()}});
    });

    server.addRoute("POST", "/api/groups/{gid}/messages",
                    [&users, &chat](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!support::authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        json body;
                        if (!support::parseJsonObject(request, &body, &error)) {
                            return error;
                        }
                        std::string content;
                        if (!support::requireString(body, "content", &content, &error)) {
                            return error;
                        }
                        std::optional<std::string> client_msg_id;
                        if (!optionalClientMessageId(body, &client_msg_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::MessageRecord message;
                        if (!chat.sendMessage(context.user.id, group_id, content, client_msg_id,
                                              &message, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", messageJson(message));
                    });

    server.addRoute("GET", "/api/groups/{gid}/messages",
                    [&users, &chat](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!support::authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        MessageListInput input;
                        if (!optionalQueryId(request, "cursor", &input.cursor, &error) ||
                            !optionalQueryId(request, "after_id", &input.after_id, &error) ||
                            !queryLimit(request, 20, &input.limit, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        MessagePage page;
                        if (!chat.listMessages(context.user.id, group_id, input, &page,
                                               &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json next_cursor = nullptr;
                        if (page.next_cursor.has_value()) {
                            next_cursor = *page.next_cursor;
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"items", messageListJson(page.items)},
                                 {"next_cursor", std::move(next_cursor)},
                                 {"server_max_id", page.server_max_id}});
                    });

    server.addAsyncRoute(
        "GET", "/api/groups/{gid}/messages/poll",
        [&users, &chat](HttpRequest& request, const AsyncContext& async_context) {
            HttpResponse error;
            AuthContext context;
            if (!support::authenticateRequest(request, users, &context, &error)) {
                async_context.respond(error);
                return;
            }
            std::uint64_t group_id = 0;
            if (!pathId(request, "gid", &group_id, &error)) {
                async_context.respond(error);
                return;
            }
            std::optional<std::uint64_t> after_id;
            std::uint64_t limit = 100;
            int timeout_seconds = 30;
            if (!optionalQueryId(request, "after_id", &after_id, &error) ||
                !queryLimit(request, 100, &limit, &error) ||
                !queryTimeoutSeconds(request, &timeout_seconds, &error)) {
                async_context.respond(error);
                return;
            }
            ServiceError service_error;
            const std::uint64_t initial_version = chat.groupVersion(group_id);
            PollSnapshot initial;
            if (!chat.pollSnapshot(context.user.id, group_id, after_id, limit, &initial,
                                   &service_error)) {
                async_context.respond(support::errorResult(service_error));
                return;
            }
            if (!initial.items.empty()) {
                async_context.respond(support::jsonResult(
                    200, 0, "ok",
                    json{{"items", messageListJson(initial.items)},
                         {"server_max_id", initial.server_max_id}}));
                return;
            }

            auto operation = std::make_shared<PollOperation>(
                chat, async_context, context.user.id, group_id, initial.effective_after_id,
                limit, timeout_seconds, initial_version);
            operation->start();
        });

    server.addRoute("POST", "/api/groups/{gid}/messages/read",
                    [&users, &chat](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!support::authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        json body;
                        if (!support::parseJsonObject(request, &body, &error)) {
                            return error;
                        }
                        std::int64_t raw_id = 0;
                        if (!support::requireInt64(body, "last_read_message_id", &raw_id,
                                                   &error)) {
                            return error;
                        }
                        if (raw_id < 0) {
                            return support::badRequest("last_read_message_id 不能为负数");
                        }
                        ServiceError service_error;
                        UnreadState state;
                        if (!chat.markRead(context.user.id, group_id,
                                           static_cast<std::uint64_t>(raw_id), &state,
                                           &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"last_read_message_id", state.last_read_message_id},
                                 {"unread_count", state.unread_count},
                                 {"max_message_id", state.max_message_id}});
                    });

    server.addRoute("GET", "/api/groups/{gid}/messages/unread",
                    [&users, &chat](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!support::authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        UnreadState state;
                        if (!chat.getUnread(context.user.id, group_id, &state, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"unread_count", state.unread_count},
                                 {"last_read_message_id", state.last_read_message_id},
                                 {"max_message_id", state.max_message_id}});
                    });
}

}  // namespace api
}  // namespace cofinance
