#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cofinance/http/HttpTypes.h"

namespace cofinance {
namespace db {
struct User;
}
namespace service {
struct ServiceError;
struct AuthContext;
class UserService;
}
namespace api {
namespace support {

using json = nlohmann::json;

http::HttpResponse jsonResult(int status, int code, const std::string& message,
                              const json& data = json::object());
http::HttpResponse errorResult(const service::ServiceError& error);
http::HttpResponse badRequest(const std::string& message);

bool parseJsonObject(const http::HttpRequest& request, json* body, http::HttpResponse* error);
bool requireString(const json& body, const char* key, std::string* value,
                   http::HttpResponse* error);
bool optionalString(const json& body, const char* key, std::optional<std::string>* value,
                    http::HttpResponse* error);

bool bearerToken(const http::HttpRequest& request, std::string* token,
                 http::HttpResponse* error);
bool authenticateRequest(const http::HttpRequest& request, service::UserService& users,
                         service::AuthContext* context, http::HttpResponse* error);

json userJson(const db::User& user);

bool queryValue(const std::string& query, const std::string& name, std::string* value);
bool parseUInt64(const std::string& text, std::uint64_t* value);

}  // namespace support
}  // namespace api
}  // namespace cofinance
