#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cofinance {
namespace http {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> path_params;
    std::string body;
    bool keep_alive = true;

    std::string header(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool close = false;

    void setHeader(std::string name, std::string value);
    std::string serialize(bool keep_alive, bool head_only) const;

    static HttpResponse text(int status, std::string body);
    static HttpResponse json(int status, std::string body);
};

const char* reasonPhrase(int status);
std::string httpDate(std::int64_t utc_millis);

}  // namespace http
}  // namespace cofinance
