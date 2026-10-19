#include "cofinance/api/UserApi.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using json = nlohmann::json;
using http::HttpResponse;
using support::authenticateRequest;
using support::badRequest;
using support::bearerToken;
using support::errorResult;
using support::jsonResult;
using support::optionalString;
using support::parseJsonObject;
using support::queryValue;
using support::requireString;
using support::userJson;

}  // namespace

void UserApi::registerRoutes(http::HttpServer& server, service::UserService& users) {
    server.addRoute("POST", "/api/auth/register", [&users](const http::HttpRequest& request) {
        json body;
        HttpResponse error;
        if (!parseJsonObject(request, &body, &error)) {
            return error;
        }
        std::string username;
        std::string password;
        std::string nickname;
        if (!requireString(body, "username", &username, &error) ||
            !requireString(body, "password", &password, &error)) {
            return error;
        }
        std::optional<std::string> optional_nickname;
        if (!optionalString(body, "nickname", &optional_nickname, &error)) {
            return error;
        }
        nickname = optional_nickname.value_or("");

        service::ServiceError service_error;
        db::User user;
        if (!users.registerUser(username, password, nickname, &user, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok", userJson(user));
    });

    server.addRoute("POST", "/api/auth/login", [&users](const http::HttpRequest& request) {
        json body;
        HttpResponse error;
        if (!parseJsonObject(request, &body, &error)) {
            return error;
        }
        std::string username;
        std::string password;
        if (!requireString(body, "username", &username, &error) ||
            !requireString(body, "password", &password, &error)) {
            return error;
        }
        service::ServiceError service_error;
        service::LoginResult result;
        if (!users.login(username, password, &result, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok",
                          json{{"token", result.token},
                               {"expires_at", result.expires_at},
                               {"user", userJson(result.user)}});
    });

    server.addRoute("POST", "/api/auth/logout", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        std::string token;
        if (!bearerToken(request, &token, &error)) {
            return error;
        }
        service::ServiceError service_error;
        if (!users.logout(token, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok");
    });

    server.addRoute("POST", "/api/auth/logout_all", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        service::AuthContext context;
        if (!authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        service::ServiceError service_error;
        if (!users.logoutAll(context, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok");
    });

    server.addRoute("GET", "/api/users/me", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        service::AuthContext context;
        if (!authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        context.user.password_hash.clear();
        return jsonResult(200, 0, "ok", userJson(context.user));
    });

    server.addRoute("PUT", "/api/users/me", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        service::AuthContext context;
        if (!authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        json body;
        if (!parseJsonObject(request, &body, &error)) {
            return error;
        }
        std::optional<std::string> nickname;
        std::optional<std::string> avatar_url;
        if (!optionalString(body, "nickname", &nickname, &error) ||
            !optionalString(body, "avatar_url", &avatar_url, &error)) {
            return error;
        }
        service::ServiceError service_error;
        db::User updated;
        if (!users.updateProfile(context.user.id, nickname, avatar_url, &updated, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok", userJson(updated));
    });

    server.addRoute("GET", "/api/users/search", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        service::AuthContext context;
        if (!authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        std::string query;
        if (!queryValue(request.query, "q", &query)) {
            return badRequest("缺少查询参数 q");
        }
        service::ServiceError service_error;
        std::vector<db::User> found;
        if (!users.searchUsers(context.user.id, query, &found, &service_error)) {
            return errorResult(service_error);
        }
        json items = json::array();
        for (const db::User& user : found) {
            items.push_back(userJson(user));
        }
        return jsonResult(200, 0, "ok", json{{"items", std::move(items)}});
    });

    server.addRoute("PUT", "/api/users/me/password",
                    [&users](const http::HttpRequest& request) {
                        HttpResponse error;
                        service::AuthContext context;
                        if (!authenticateRequest(request, users, &context, &error)) {
                            return error;
                        }
                        json body;
                        if (!parseJsonObject(request, &body, &error)) {
                            return error;
                        }
                        std::string old_password;
                        std::string new_password;
                        if (!requireString(body, "old_password", &old_password, &error) ||
                            !requireString(body, "new_password", &new_password, &error)) {
                            return error;
                        }
                        service::ServiceError service_error;
                        if (!users.changePassword(context, old_password, new_password,
                                                  &service_error)) {
                            return errorResult(service_error);
                        }
                        return jsonResult(200, 0, "ok");
                    });

    server.addRoute("DELETE", "/api/users/me", [&users](const http::HttpRequest& request) {
        HttpResponse error;
        service::AuthContext context;
        if (!authenticateRequest(request, users, &context, &error)) {
            return error;
        }
        service::ServiceError service_error;
        if (!users.deleteAccount(context, &service_error)) {
            return errorResult(service_error);
        }
        return jsonResult(200, 0, "ok");
    });
}

}  // namespace api
}  // namespace cofinance
