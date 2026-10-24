#include "cofinance/http/Router.h"

#include <algorithm>
#include <exception>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace http {

void Router::add(std::string method, std::string pattern, Handler handler) {
    Route route;
    route.method = util::toUpper(std::move(method));
    route.segments = splitPath(pattern);
    route.handler = std::move(handler);
    routes_.push_back(std::move(route));
}

void Router::addAsync(std::string method, std::string pattern, AsyncHandler handler) {
    Route route;
    route.method = util::toUpper(std::move(method));
    route.segments = splitPath(pattern);
    route.async_handler = std::move(handler);
    routes_.push_back(std::move(route));
}

void Router::addUpgrade(std::string pattern, UpgradeHandler handler) {
    UpgradeRoute route;
    route.segments = splitPath(pattern);
    route.handler = std::move(handler);
    upgrade_routes_.push_back(std::move(route));
}

std::optional<UpgradeDecision> Router::dispatchUpgrade(
    HttpRequest& request, const UpgradeContext& context) const {
    if (request.method != "GET") {
        return std::nullopt;
    }
    const std::vector<std::string> actual = splitPath(request.path);
    for (const UpgradeRoute& route : upgrade_routes_) {
        if (!matchSegments(route.segments, actual, &request)) {
            continue;
        }
        try {
            return route.handler(request, context);
        } catch (const std::exception& e) {
            LOG_ERROR("HTTP Upgrade 路由异常 %s: %s", request.path.c_str(), e.what());
        } catch (...) {
            LOG_ERROR("HTTP Upgrade 路由未知异常 %s", request.path.c_str());
        }
        UpgradeDecision decision;
        decision.rejection = HttpResponse::text(500, "Internal Server Error\n");
        return decision;
    }
    return std::nullopt;
}

DispatchResult Router::dispatch(HttpRequest& request, const AsyncContext& context) const {
    const std::vector<std::string> actual = splitPath(request.path);
    std::set<std::string> allowed;

    for (const Route& route : routes_) {
        if (matchSegments(route.segments, actual, nullptr) && route.method != request.method &&
            !(request.method == "HEAD" && route.method == "GET")) {
            allowed.insert(route.method);
        }
    }

    for (const Route& route : routes_) {
        const bool method_matches =
            route.method == request.method || (request.method == "HEAD" && route.method == "GET");
        if (!method_matches || !matchSegments(route.segments, actual, &request)) {
            continue;
        }
        if (route.async_handler) {
            try {
                route.async_handler(request, context);
            } catch (const std::exception& e) {
                LOG_ERROR("HTTP 异步路由处理异常 %s %s: %s", request.method.c_str(),
                          request.path.c_str(), e.what());
                context.respond(HttpResponse::text(500, "Internal Server Error\n"));
            } catch (...) {
                LOG_ERROR("HTTP 异步路由处理未知异常 %s %s", request.method.c_str(),
                          request.path.c_str());
                context.respond(HttpResponse::text(500, "Internal Server Error\n"));
            }
            return DispatchResult{DispatchStatus::Pending, HttpResponse()};
        }
        try {
            return DispatchResult{DispatchStatus::Complete, route.handler(request)};
        } catch (const std::exception& e) {
            LOG_ERROR("HTTP 路由处理异常 %s %s: %s", request.method.c_str(),
                      request.path.c_str(), e.what());
            return DispatchResult{DispatchStatus::Complete,
                                  HttpResponse::text(500, "Internal Server Error\n")};
        } catch (...) {
            LOG_ERROR("HTTP 路由处理未知异常 %s %s", request.method.c_str(),
                      request.path.c_str());
            return DispatchResult{DispatchStatus::Complete,
                                  HttpResponse::text(500, "Internal Server Error\n")};
        }
    }

    if (!allowed.empty()) {
        std::string allow;
        for (const std::string& method : allowed) {
            if (!allow.empty()) {
                allow += ", ";
            }
            allow += method;
        }
        HttpResponse response = HttpResponse::text(405, "Method Not Allowed\n");
        response.setHeader("Allow", allow);
        return DispatchResult{DispatchStatus::MethodNotAllowed, std::move(response)};
    }
    return DispatchResult{};
}

std::optional<HttpResponse> Router::dispatch(HttpRequest& request) const {
    const std::vector<std::string> actual = splitPath(request.path);
    std::set<std::string> allowed;

    for (const Route& route : routes_) {
        if (matchSegments(route.segments, actual, nullptr) &&
            route.method != request.method &&
            !(request.method == "HEAD" && route.method == "GET")) {
            allowed.insert(route.method);
        }
    }

    for (const Route& route : routes_) {
        const bool method_matches =
            route.method == request.method || (request.method == "HEAD" && route.method == "GET");
        if (!method_matches || !matchSegments(route.segments, actual, &request)) {
            continue;
        }
        try {
            return route.handler(request);
        } catch (const std::exception& e) {
            LOG_ERROR("HTTP 路由处理异常 %s %s: %s", request.method.c_str(),
                      request.path.c_str(), e.what());
            return HttpResponse::text(500, "Internal Server Error\n");
        } catch (...) {
            LOG_ERROR("HTTP 路由处理未知异常 %s %s", request.method.c_str(),
                      request.path.c_str());
            return HttpResponse::text(500, "Internal Server Error\n");
        }
    }

    if (!allowed.empty()) {
        std::string allow;
        for (const std::string& method : allowed) {
            if (!allow.empty()) {
                allow += ", ";
            }
            allow += method;
        }
        HttpResponse response = HttpResponse::text(405, "Method Not Allowed\n");
        response.setHeader("Allow", allow);
        return response;
    }
    return std::nullopt;
}

std::vector<std::string> Router::splitPath(const std::string& path) {
    std::string normalized = path;
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    std::vector<std::string> segments;
    if (normalized.empty() || normalized == "/") {
        return segments;
    }
    std::size_t begin = normalized.front() == '/' ? 1 : 0;
    while (begin < normalized.size()) {
        const std::size_t slash = normalized.find('/', begin);
        const std::size_t end = slash == std::string::npos ? normalized.size() : slash;
        if (end > begin) {
            segments.push_back(normalized.substr(begin, end - begin));
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1;
    }
    return segments;
}

bool Router::matchSegments(const std::vector<std::string>& pattern,
                           const std::vector<std::string>& actual, HttpRequest* request) {
    if (pattern.size() != actual.size()) {
        return false;
    }
    std::unordered_map<std::string, std::string> params;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const std::string& expected = pattern[i];
        if (expected.size() >= 3 && expected.front() == '{' && expected.back() == '}') {
            params.emplace(expected.substr(1, expected.size() - 2), actual[i]);
            continue;
        }
        if (expected != actual[i]) {
            return false;
        }
    }
    if (request != nullptr) {
        request->path_params = std::move(params);
    }
    return true;
}

}  // namespace http
}  // namespace cofinance
