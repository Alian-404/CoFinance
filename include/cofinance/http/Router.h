#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/http/HttpTypes.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace http {

// 路由表只在启动阶段写入，运行期只读；因此多个 IO worker 可以并发 dispatch。
class Router : private NonCopyable {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void add(std::string method, std::string pattern, Handler handler);
    std::optional<HttpResponse> dispatch(HttpRequest& request) const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
    };

    static std::vector<std::string> splitPath(const std::string& path);
    static bool matchSegments(const std::vector<std::string>& pattern,
                              const std::vector<std::string>& actual,
                              HttpRequest* request);

    std::vector<Route> routes_;
};

}  // namespace http
}  // namespace cofinance
