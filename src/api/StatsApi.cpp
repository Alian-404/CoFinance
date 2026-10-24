#include "cofinance/api/StatsApi.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/SettlementService.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using json = support::json;
using http::HttpRequest;
using http::HttpResponse;
using service::AuthContext;
using service::BalanceSummary;
using service::GroupStats;
using service::ServiceError;
using service::SettlementService;
using service::SettlementSuggestion;
using service::StatsInput;
using service::UserService;

json balanceJson(const db::BalanceRecord& balance) {
    return json{{"user_id", balance.user_id},
                {"username", balance.username},
                {"nickname", balance.nickname},
                {"avatar_url", balance.avatar_url},
                {"transaction_balance_cents", balance.transaction_balance_cents},
                {"settlement_balance_cents", balance.settlement_balance_cents},
                {"balance_cents", balance.balance_cents}};
}

json settlementJson(const db::SettlementRecord& settlement) {
    json confirmed_at = nullptr;
    if (settlement.confirmed_at.has_value()) {
        confirmed_at = *settlement.confirmed_at;
    }
    return json{{"id", settlement.id},
                {"group_id", settlement.group_id},
                {"from_user_id", settlement.from_user_id},
                {"from_username", settlement.from_username},
                {"from_nickname", settlement.from_nickname},
                {"to_user_id", settlement.to_user_id},
                {"to_username", settlement.to_username},
                {"to_nickname", settlement.to_nickname},
                {"amount_cents", settlement.amount_cents},
                {"status", settlement.status},
                {"created_by", settlement.created_by},
                {"created_at", settlement.created_at},
                {"confirmed_at", std::move(confirmed_at)}};
}

json suggestionJson(const SettlementSuggestion& suggestion) {
    return json{{"from_user_id", suggestion.from_user_id},
                {"from_username", suggestion.from_username},
                {"from_nickname", suggestion.from_nickname},
                {"to_user_id", suggestion.to_user_id},
                {"to_username", suggestion.to_username},
                {"to_nickname", suggestion.to_nickname},
                {"amount_cents", suggestion.amount_cents}};
}

json categoryStatJson(const db::CategoryStatRecord& category, std::int64_t total_cents) {
    double ratio = 0.0;
    if (total_cents > 0) {
        ratio = static_cast<double>(category.amount_cents) * 100.0 /
                static_cast<double>(total_cents);
    }
    return json{{"category_id", category.category_id},
                {"name", category.category_name},
                {"amount_cents", category.amount_cents},
                {"ratio", ratio}};
}

json memberStatJson(const db::MemberStatRecord& member) {
    const std::int64_t transaction_balance =
        (member.paid_expense_cents - member.share_expense_cents) +
        (member.share_income_cents - member.paid_income_cents);
    return json{{"user_id", member.user_id},
                {"username", member.username},
                {"nickname", member.nickname},
                {"avatar_url", member.avatar_url},
                {"paid_expense_cents", member.paid_expense_cents},
                {"paid_income_cents", member.paid_income_cents},
                {"share_expense_cents", member.share_expense_cents},
                {"share_income_cents", member.share_income_cents},
                {"transaction_balance_cents", transaction_balance}};
}

json statsJson(const GroupStats& stats) {
    json from = nullptr;
    json to = nullptr;
    if (stats.from.has_value()) {
        from = *stats.from;
    }
    if (stats.to.has_value()) {
        to = *stats.to;
    }
    json expense_categories = json::array();
    json income_categories = json::array();
    for (const db::CategoryStatRecord& category : stats.categories) {
        if (category.type == 0) {
            expense_categories.push_back(
                categoryStatJson(category, stats.summary.total_expense_cents));
        } else {
            income_categories.push_back(
                categoryStatJson(category, stats.summary.total_income_cents));
        }
    }
    json members = json::array();
    for (const db::MemberStatRecord& member : stats.members) {
        members.push_back(memberStatJson(member));
    }
    return json{{"from", std::move(from)},
                {"to", std::move(to)},
                {"summary",
                 {{"total_expense_cents", stats.summary.total_expense_cents},
                  {"total_income_cents", stats.summary.total_income_cents},
                  {"net_expense_cents", stats.summary.total_expense_cents -
                                            stats.summary.total_income_cents},
                  {"transaction_count", stats.summary.transaction_count}}},
                {"expense_categories", std::move(expense_categories)},
                {"income_categories", std::move(income_categories)},
                {"members", std::move(members)}};
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

bool queryText(const HttpRequest& request, const char* name, std::optional<std::string>* value,
               HttpResponse* error) {
    std::string raw;
    bool found = false;
    if (!support::queryValue(request.query, name, &raw, &found)) {
        *error = support::badRequest(std::string("查询参数编码无效: ") + name);
        return false;
    }
    if (found) {
        if (raw.empty()) {
            *error = support::badRequest(std::string("查询参数不能为空: ") + name);
            return false;
        }
        *value = raw;
    } else {
        value->reset();
    }
    return true;
}

bool readPositiveId(const json& body, const char* key, std::uint64_t* value,
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

}  // namespace

void StatsApi::registerRoutes(http::HttpServer& server, UserService& users,
                              SettlementService& settlements) {
    server.addRoute("GET", "/api/groups/{gid}/stats",
                    [&users, &settlements](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!support::authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        StatsInput input;
                        if (!queryText(request, "from", &input.from, &error) ||
                            !queryText(request, "to", &input.to, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        GroupStats stats;
                        if (!settlements.getStats(context.user.id, group_id, input, &stats,
                                                  &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", statsJson(stats));
                    });

    server.addRoute("GET", "/api/groups/{gid}/balances",
                    [&users, &settlements](const HttpRequest& request) {
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
                        BalanceSummary balances;
                        if (!settlements.getBalances(context.user.id, group_id, &balances,
                                                     &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json items = json::array();
                        for (const db::BalanceRecord& balance : balances.items) {
                            items.push_back(balanceJson(balance));
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"items", std::move(items)},
                                 {"total_balance_cents", balances.total_balance_cents}});
                    });

    server.addRoute("POST", "/api/groups/{gid}/settlements/suggest",
                    [&users, &settlements](const HttpRequest& request) {
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
                        std::vector<SettlementSuggestion> suggestions;
                        if (!settlements.suggestSettlements(context.user.id, group_id,
                                                           &suggestions, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json items = json::array();
                        for (const SettlementSuggestion& suggestion : suggestions) {
                            items.push_back(suggestionJson(suggestion));
                        }
                        return support::jsonResult(
                            200, 0, "ok",
                            json{{"items", std::move(items)}, {"count", suggestions.size()}});
                    });

    server.addRoute("POST", "/api/groups/{gid}/settlements",
                    [&users, &settlements](const HttpRequest& request) {
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
                        std::uint64_t from_user_id = 0;
                        std::uint64_t to_user_id = 0;
                        std::int64_t amount_cents = 0;
                        if (!readPositiveId(body, "from_user_id", &from_user_id, &error) ||
                            !readPositiveId(body, "to_user_id", &to_user_id, &error) ||
                            !support::requireInt64(body, "amount_cents", &amount_cents, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::SettlementRecord settlement;
                        if (!settlements.confirmSettlement(context.user.id, group_id,
                                                          from_user_id, to_user_id,
                                                          amount_cents, &settlement,
                                                          &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", settlementJson(settlement));
                    });

    server.addRoute("GET", "/api/groups/{gid}/settlements",
                    [&users, &settlements](const HttpRequest& request) {
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
                        std::vector<db::SettlementRecord> records;
                        if (!settlements.listSettlements(context.user.id, group_id, &records,
                                                         &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json items = json::array();
                        for (const db::SettlementRecord& record : records) {
                            items.push_back(settlementJson(record));
                        }
                        return support::jsonResult(200, 0, "ok",
                                                   json{{"items", std::move(items)}});
                    });
}

}  // namespace api
}  // namespace cofinance
