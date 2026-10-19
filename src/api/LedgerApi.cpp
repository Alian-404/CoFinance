#include "cofinance/api/LedgerApi.h"

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/LedgerService.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using json = support::json;
using http::HttpRequest;
using http::HttpResponse;
using service::AuthContext;
using service::LedgerService;
using service::ServiceError;
using service::TransactionFilterInput;
using service::TransactionInput;
using service::UserService;

json categoryJson(const db::CategoryRecord& category) {
    return json{{"id", category.id},
                {"group_id", category.group_id},
                {"name", category.name},
                {"icon", category.icon},
                {"sort", category.sort}};
}

json participantJson(const db::ParticipantRecord& participant) {
    return json{{"user_id", participant.user_id},
                {"username", participant.username},
                {"nickname", participant.nickname},
                {"avatar_url", participant.avatar_url},
                {"share_cents", participant.share_cents}};
}

json transactionJson(const db::TransactionRecord& transaction) {
    json client_token = nullptr;
    if (transaction.client_token.has_value()) {
        client_token = *transaction.client_token;
    }
    return json{{"id", transaction.id},
                {"group_id", transaction.group_id},
                {"type", transaction.type},
                {"amount_cents", transaction.amount_cents},
                {"currency", transaction.currency},
                {"category_id", transaction.category_id},
                {"category_name", transaction.category_name},
                {"note", transaction.note},
                {"payer_id", transaction.payer_id},
                {"payer_nickname", transaction.payer_nickname},
                {"split_type", transaction.split_type},
                {"client_token", std::move(client_token)},
                {"created_by", transaction.created_by},
                {"created_at", transaction.created_at},
                {"updated_at", transaction.updated_at},
                {"deleted", transaction.deleted}};
}

json transactionJson(const db::TransactionDetail& detail) {
    json value = transactionJson(detail.transaction);
    json participants = json::array();
    for (const db::ParticipantRecord& participant : detail.participants) {
        participants.push_back(participantJson(participant));
    }
    value["participants"] = std::move(participants);
    return value;
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

bool authenticate(const HttpRequest& request, UserService& users, AuthContext* context,
                  HttpResponse* error) {
    return support::authenticateRequest(request, users, context, error);
}

bool readIdField(const json& body, const char* key, std::uint64_t* value,
                 HttpResponse* error) {
    std::int64_t raw = 0;
    if (!support::requireInt64(body, key, &raw, error)) {
        return false;
    }
    if (raw <= 0) {
        *error = support::badRequest(std::string("字段必须是正整数: ") + key);
        return false;
    }
    *value = static_cast<std::uint64_t>(raw);
    return true;
}

bool optionalIdField(const json& body, const char* key, std::uint64_t* value,
                     HttpResponse* error) {
    std::optional<std::int64_t> raw;
    if (!support::optionalInt64(body, key, &raw, error)) {
        return false;
    }
    if (!raw.has_value()) {
        *value = 0;
        return true;
    }
    if (*raw < 0) {
        *error = support::badRequest(std::string("字段不能为负数: ") + key);
        return false;
    }
    *value = static_cast<std::uint64_t>(*raw);
    return true;
}

bool parseIntField(const json& body, const char* key, int default_value, int* value,
                   HttpResponse* error) {
    std::optional<std::int64_t> raw;
    if (!support::optionalInt64(body, key, &raw, error)) {
        return false;
    }
    if (!raw.has_value()) {
        *value = default_value;
        return true;
    }
    if (*raw < INT_MIN || *raw > INT_MAX) {
        *error = support::badRequest(std::string("字段超出整数范围: ") + key);
        return false;
    }
    *value = static_cast<int>(*raw);
    return true;
}

bool requireIntField(const json& body, const char* key, int* value, HttpResponse* error) {
    std::int64_t raw = 0;
    if (!support::requireInt64(body, key, &raw, error)) {
        return false;
    }
    if (raw < INT_MIN || raw > INT_MAX) {
        *error = support::badRequest(std::string("字段超出整数范围: ") + key);
        return false;
    }
    *value = static_cast<int>(raw);
    return true;
}

bool parseParticipants(const json& body,
                       std::vector<service::ParticipantInput>* participants,
                       HttpResponse* error) {
    const auto found = body.find("participants");
    if (found == body.end() || !found->is_array()) {
        *error = support::badRequest("participants 必须是数组");
        return false;
    }
    participants->clear();
    for (const json& item : *found) {
        service::ParticipantInput participant;
        if (item.is_number_integer() || item.is_number_unsigned()) {
            std::int64_t raw_id = 0;
            if (!support::parseInt64Value(item, &raw_id) || raw_id <= 0) {
                *error = support::badRequest("participants 中的用户 ID 非法");
                return false;
            }
            participant.user_id = static_cast<std::uint64_t>(raw_id);
            participants->push_back(participant);
            continue;
        }
        if (!item.is_object()) {
            *error = support::badRequest("participants 元素必须是 ID 或对象");
            return false;
        }
        if (!readIdField(item, "user_id", &participant.user_id, error)) {
            return false;
        }
        std::optional<std::int64_t> share;
        if (!support::optionalInt64(item, "share_cents", &share, error)) {
            return false;
        }
        participant.share_cents = share;
        participants->push_back(participant);
    }
    return true;
}

bool parseTransactionInput(const HttpRequest& request, bool allow_client_token,
                           TransactionInput* input, HttpResponse* error) {
    json body;
    if (!support::parseJsonObject(request, &body, error)) {
        return false;
    }
    if (!requireIntField(body, "type", &input->type, error) ||
        !support::requireInt64(body, "amount_cents", &input->amount_cents, error) ||
        !readIdField(body, "payer_id", &input->payer_id, error) ||
        !optionalIdField(body, "category_id", &input->category_id, error) ||
        !parseIntField(body, "split_type", 0, &input->split_type, error)) {
        return false;
    }
    std::optional<std::string> note;
    if (!support::optionalString(body, "note", &note, error)) {
        return false;
    }
    input->note = note.value_or("");
    if (!parseParticipants(body, &input->participants, error)) {
        return false;
    }
    if (allow_client_token) {
        if (!support::optionalString(body, "client_token", &input->client_token, error)) {
            return false;
        }
    } else {
        input->client_token.reset();
    }
    return true;
}

bool optionalQueryId(const HttpRequest& request, const char* name,
                     std::optional<std::uint64_t>* value, HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, name, &raw, &found)) {
        *error = support::badRequest(std::string("查询参数格式错误: ") + name);
        return false;
    }
    if (!found) {
        value->reset();
        return true;
    }
    std::uint64_t parsed = 0;
    if (!support::parseUInt64(raw, &parsed)) {
        *error = support::badRequest(std::string("查询参数必须是正整数: ") + name);
        return false;
    }
    *value = parsed;
    return true;
}

bool optionalQueryTime(const HttpRequest& request, const char* name,
                       std::optional<std::string>* value, HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, name, &raw, &found)) {
        *error = support::badRequest(std::string("查询参数格式错误: ") + name);
        return false;
    }
    if (!found) {
        value->reset();
        return true;
    }
    *value = raw;
    return true;
}

bool parseFilter(const HttpRequest& request, TransactionFilterInput* filter,
                 HttpResponse* error) {
    if (!optionalQueryId(request, "cursor", &filter->cursor, error) ||
        !optionalQueryId(request, "category_id", &filter->category_id, error) ||
        !optionalQueryId(request, "payer_id", &filter->payer_id, error) ||
        !optionalQueryTime(request, "from", &filter->from, error) ||
        !optionalQueryTime(request, "to", &filter->to, error)) {
        return false;
    }
    std::string raw_limit;
    bool has_limit = false;
    if (!support::queryValue(request.query, "limit", &raw_limit, &has_limit)) {
        *error = support::badRequest("查询参数格式错误: limit");
        return false;
    }
    if (has_limit) {
        if (!support::parseUInt64(raw_limit, &filter->limit) || filter->limit > 100) {
            *error = support::badRequest("limit 必须在 1-100 之间");
            return false;
        }
    }
    return true;
}

}  // namespace

void LedgerApi::registerRoutes(http::HttpServer& server, UserService& users,
                               LedgerService& ledger) {
    server.addRoute("POST", "/api/groups/{gid}/transactions",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        TransactionInput input;
                        if (!parseTransactionInput(request, true, &input, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::TransactionDetail detail;
                        if (!ledger.createTransaction(context.user.id, group_id, input, &detail,
                                                      &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", transactionJson(detail));
                    });

    server.addRoute("GET", "/api/groups/{gid}/transactions",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        TransactionFilterInput filter;
                        if (!parseFilter(request, &filter, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        service::TransactionPage page;
                        if (!ledger.listTransactions(context.user.id, group_id, filter, &page,
                                                     &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json items = json::array();
                        for (const db::TransactionRecord& transaction : page.items) {
                            items.push_back(transactionJson(transaction));
                        }
                        json next_cursor = nullptr;
                        if (page.next_cursor.has_value()) {
                            next_cursor = *page.next_cursor;
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"items", std::move(items)}, {"next_cursor", next_cursor}});
                    });

    server.addRoute("GET", "/api/groups/{gid}/transactions/{tid}",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t transaction_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "tid", &transaction_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::TransactionDetail detail;
                        if (!ledger.getTransaction(context.user.id, group_id, transaction_id,
                                                   &detail, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", transactionJson(detail));
                    });

    server.addRoute("PUT", "/api/groups/{gid}/transactions/{tid}",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t transaction_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "tid", &transaction_id, &error)) {
                            return error;
                        }
                        TransactionInput input;
                        if (!parseTransactionInput(request, false, &input, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::TransactionDetail detail;
                        if (!ledger.updateTransaction(context.user.id, group_id, transaction_id,
                                                      input, &detail, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", transactionJson(detail));
                    });

    server.addRoute("DELETE", "/api/groups/{gid}/transactions/{tid}",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t transaction_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "tid", &transaction_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!ledger.deleteTransaction(context.user.id, group_id, transaction_id,
                                                      &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });

    server.addRoute("GET", "/api/categories", [&users, &ledger](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        std::optional<std::uint64_t> group_id;
        if (!optionalQueryId(request, "gid", &group_id, &error)) {
            return error;
        }
        ServiceError service_error;
        std::vector<db::CategoryRecord> categories;
        if (!ledger.listCategories(context.user.id, group_id, &categories, &service_error)) {
            return support::errorResult(service_error);
        }
        json items = json::array();
        for (const db::CategoryRecord& category : categories) {
            items.push_back(categoryJson(category));
        }
        return support::jsonResult(200, 0, "ok", json{{"items", std::move(items)}});
    });

    server.addRoute("POST", "/api/groups/{gid}/categories",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
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
                        std::string name;
                        if (!support::requireString(body, "name", &name, &error)) {
                            return error;
                        }
                        std::optional<std::string> icon;
                        if (!support::optionalString(body, "icon", &icon, &error)) {
                            return error;
                        }
                        int sort = 0;
                        if (!parseIntField(body, "sort", 100, &sort, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::CategoryRecord category;
                        if (!ledger.createCategory(context.user.id, group_id, name,
                                                   icon.value_or(""), sort, &category,
                                                   &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", categoryJson(category));
                    });

    server.addRoute("DELETE", "/api/groups/{gid}/categories/{cid}",
                    [&users, &ledger](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t category_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "cid", &category_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!ledger.deleteCategory(context.user.id, group_id, category_id,
                                                   &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });
}

}  // namespace api
}  // namespace cofinance
