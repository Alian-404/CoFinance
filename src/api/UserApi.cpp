#include "cofinance/api/UserApi.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/http/HttpServer.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace api {
namespace {

using json = nlohmann::json;
using http::HttpResponse;

HttpResponse jsonResult(int status, int code, const std::string& message,
                        const json& data = json::object()) {
    const json body{{"code", code}, {"msg", message}, {"data", data}};
    return HttpResponse::json(status, body.dump());
}

HttpResponse errorResult(const service::ServiceError& error) {
    return jsonResult(error.http_status, error.code, error.message);
}

HttpResponse badRequest(const std::string& message) {
    return jsonResult(400, 1001, message);
}

bool parseJsonObject(const http::HttpRequest& request, json* body, HttpResponse* error) {
    json parsed = json::parse(request.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        *error = badRequest("请求体必须是 JSON 对象");
        return false;
    }
    *body = std::move(parsed);
    return true;
}

bool requireString(const json& body, const char* key, std::string* value, HttpResponse* error) {
    const auto found = body.find(key);
    if (found == body.end() || !found->is_string()) {
        *error = badRequest(std::string("缺少字符串字段: ") + key);
        return false;
    }
    *value = found->get<std::string>();
    return true;
}

bool optionalString(const json& body, const char* key, std::optional<std::string>* value,
                    HttpResponse* error) {
    const auto found = body.find(key);
    if (found == body.end()) {
        value->reset();
        return true;
    }
    if (!found->is_string()) {
        *error = badRequest(std::string("字段必须是字符串: ") + key);
        return false;
    }
    *value = found->get<std::string>();
    return true;
}

bool bearerToken(const http::HttpRequest& request, std::string* token, HttpResponse* error) {
    const std::string header = request.header("authorization");
    if (header.size() <= 7 || !util::iequals(header.substr(0, 7), "Bearer ")) {
        *error = jsonResult(401, 1002, "未登录或 token 无效");
        return false;
    }
    *token = util::trim(header.substr(7));
    if (token->empty()) {
        *error = jsonResult(401, 1002, "未登录或 token 无效");
        return false;
    }
    return true;
}

bool authenticateRequest(const http::HttpRequest& request, service::UserService& users,
                         service::AuthContext* context, HttpResponse* error) {
    std::string token;
    if (!bearerToken(request, &token, error)) {
        return false;
    }
    service::ServiceError service_error;
    if (!users.authenticate(token, context, &service_error)) {
        *error = errorResult(service_error);
        return false;
    }
    return true;
}

json userJson(const db::User& user) {
    return json{{"id", user.id},
                {"username", user.username},
                {"nickname", user.nickname},
                {"avatar_url", user.avatar_url},
                {"status", user.status}};
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool decodeQueryPart(const std::string& raw, std::string* decoded) {
    decoded->clear();
    decoded->reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '+') {
            decoded->push_back(' ');
            continue;
        }
        if (raw[i] != '%') {
            decoded->push_back(raw[i]);
            continue;
        }
        if (i + 2 >= raw.size()) {
            return false;
        }
        const int hi = hexDigit(raw[i + 1]);
        const int lo = hexDigit(raw[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        decoded->push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

bool queryValue(const std::string& query, const std::string& name, std::string* value) {
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const std::size_t amp = query.find('&', begin);
        const std::string pair = query.substr(
            begin, amp == std::string::npos ? std::string::npos : amp - begin);
        const std::size_t equals = pair.find('=');
        const std::string raw_key = pair.substr(0, equals);
        std::string key;
        if (decodeQueryPart(raw_key, &key) && key == name) {
            const std::string raw_value =
                equals == std::string::npos ? std::string() : pair.substr(equals + 1);
            return decodeQueryPart(raw_value, value);
        }
        if (amp == std::string::npos) {
            break;
        }
        begin = amp + 1;
    }
    return false;
}

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
