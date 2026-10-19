#include "cofinance/api/ApiSupport.h"

#include <limits>
#include <string>
#include <utility>

#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace api {
namespace support {

http::HttpResponse jsonResult(int status, int code, const std::string& message,
                              const json& data) {
    const json body{{"code", code}, {"msg", message}, {"data", data}};
    return http::HttpResponse::json(status, body.dump());
}

http::HttpResponse errorResult(const service::ServiceError& error) {
    return jsonResult(error.http_status, error.code, error.message);
}

http::HttpResponse badRequest(const std::string& message) {
    return jsonResult(400, 1001, message);
}

bool parseJsonObject(const http::HttpRequest& request, json* body, http::HttpResponse* error) {
    json parsed = json::parse(request.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        *error = badRequest("请求体必须是 JSON 对象");
        return false;
    }
    *body = std::move(parsed);
    return true;
}

bool requireString(const json& body, const char* key, std::string* value,
                   http::HttpResponse* error) {
    const auto found = body.find(key);
    if (found == body.end() || !found->is_string()) {
        *error = badRequest(std::string("缺少字符串字段: ") + key);
        return false;
    }
    *value = found->get<std::string>();
    return true;
}

bool optionalString(const json& body, const char* key, std::optional<std::string>* value,
                    http::HttpResponse* error) {
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

bool bearerToken(const http::HttpRequest& request, std::string* token,
                 http::HttpResponse* error) {
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
                         service::AuthContext* context, http::HttpResponse* error) {
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

bool parseUInt64(const std::string& text, std::uint64_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed == 0) {
        return false;
    }
    *value = parsed;
    return true;
}

}  // namespace support
}  // namespace api
}  // namespace cofinance
