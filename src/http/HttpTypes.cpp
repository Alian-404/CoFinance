#include "cofinance/http/HttpTypes.h"

#include <ctime>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cofinance {
namespace http {
namespace {

bool safeHeaderText(const std::string& text) {
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
    std::string lower = name;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto found = headers.find(lower);
    return found == headers.end() ? std::string() : found->second;
}

void HttpResponse::setHeader(std::string name, std::string value) {
    for (auto& item : headers) {
        if (item.first == name) {
            item.second = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::move(name), std::move(value));
}

std::string HttpResponse::serialize(bool keep_alive, bool head_only) const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out.push_back(' ');
    out += reasonPhrase(status);
    out += "\r\n";
    out += "Server: CoFinance\r\n";
    out += "Date: ";
    out += httpDate(static_cast<std::int64_t>(std::time(nullptr)) * 1000);
    out += "\r\n";
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += "Content-Type: ";
    out += content_type;
    out += "\r\n";
    out += "Connection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& item : headers) {
        if (!safeHeaderText(item.first) || !safeHeaderText(item.second)) {
            continue;
        }
        out += item.first;
        out += ": ";
        out += item.second;
        out += "\r\n";
    }
    out += "\r\n";
    if (!head_only) {
        out += body;
    }
    return out;
}

HttpResponse HttpResponse::text(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::json(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.body = std::move(body);
    return response;
}

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string httpDate(std::int64_t utc_millis) {
    const std::time_t seconds = static_cast<std::time_t>(utc_millis / 1000);
    std::tm tm_value {};
    if (::gmtime_r(&seconds, &tm_value) == nullptr) {
        return "Thu, 01 Jan 1970 00:00:00 GMT";
    }
    char buffer[64] {};
    if (std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_value) == 0) {
        return "Thu, 01 Jan 1970 00:00:00 GMT";
    }
    return buffer;
}

}  // namespace http
}  // namespace cofinance
