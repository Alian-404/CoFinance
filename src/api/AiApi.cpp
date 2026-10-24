#include "cofinance/api/AiApi.h"

#include <cstdint>
#include <string>
#include <utility>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/AiService.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using http::AsyncContext;
using http::HttpRequest;
using http::HttpResponse;
using service::AiResult;
using service::AiService;
using service::AuthContext;
using service::ServiceError;
using service::UserService;
using support::json;

bool pathId(const HttpRequest& request, const char* name, std::uint64_t* value,
            HttpResponse* error) {
    const auto found = request.path_params.find(name);
    if (found == request.path_params.end() || !support::parseUInt64(found->second, value)) {
        *error = support::badRequest(std::string("路径参数必须是正整数: ") + name);
        return false;
    }
    return true;
}

HttpResponse answerResponse(const AiResult& result) {
    if (!result.ok) {
        return support::errorResult(result.error);
    }
    return support::jsonResult(
        200, 0, "ok",
        json{{"answer", result.answer.answer},
             {"cached", result.answer.cached},
             {"elapsed_ms", result.answer.elapsed_ms},
             {"model", result.answer.model},
             {"period",
              {{"from", result.answer.current_from}, {"to", result.answer.current_to}}}});
}

void respondLater(const AsyncContext& context, AiResult result) {
    context.respond(answerResponse(result));
}

}  // namespace

void AiApi::registerRoutes(http::HttpServer& server, UserService& users, AiService& ai) {
    server.addAsyncRoute(
        "POST", "/api/groups/{gid}/ai/ask",
        [&users, &ai](HttpRequest& request, const AsyncContext& async_context) {
            HttpResponse error;
            AuthContext auth;
            if (!support::authenticateRequest(request, users, &auth, &error)) {
                async_context.respond(error);
                return;
            }
            std::uint64_t group_id = 0;
            if (!pathId(request, "gid", &group_id, &error)) {
                async_context.respond(error);
                return;
            }
            json body;
            if (!support::parseJsonObject(request, &body, &error)) {
                async_context.respond(error);
                return;
            }
            std::string question;
            if (!support::requireString(body, "question", &question, &error)) {
                async_context.respond(error);
                return;
            }

            ServiceError service_error;
            std::shared_ptr<service::AiCancellation> cancellation = ai.askAsync(
                auth.user.id, group_id, std::move(question),
                [async_context](AiResult result) { respondLater(async_context, std::move(result)); },
                &service_error);
            if (!cancellation) {
                async_context.respond(support::errorResult(service_error));
                return;
            }
            async_context.set_cancel([cancellation]() { cancellation->cancel(); });
        });

    server.addAsyncRoute(
        "GET", "/api/groups/{gid}/ai/suggestions",
        [&users, &ai](HttpRequest& request, const AsyncContext& async_context) {
            HttpResponse error;
            AuthContext auth;
            if (!support::authenticateRequest(request, users, &auth, &error)) {
                async_context.respond(error);
                return;
            }
            std::uint64_t group_id = 0;
            if (!pathId(request, "gid", &group_id, &error)) {
                async_context.respond(error);
                return;
            }

            ServiceError service_error;
            std::shared_ptr<service::AiCancellation> cancellation = ai.suggestionsAsync(
                auth.user.id, group_id,
                [async_context](AiResult result) { respondLater(async_context, std::move(result)); },
                &service_error);
            if (!cancellation) {
                async_context.respond(support::errorResult(service_error));
                return;
            }
            async_context.set_cancel([cancellation]() { cancellation->cancel(); });
        });
}

}  // namespace api
}  // namespace cofinance
