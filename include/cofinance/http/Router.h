#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/http/HttpTypes.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace http {

// 异步处理器在请求所属 IO 线程被调用。respond 可以从任意线程调用一次；
// post/add_timer/cancel_timer 都会回到该连接所属 EventLoop 执行。
struct AsyncContext {
    std::function<void(HttpResponse)> respond;
    std::function<void(std::function<void()>)> set_cancel;
    std::function<void(std::function<void()>)> post;
    std::function<std::uint64_t(std::int64_t, std::function<void()>)> add_timer;
    std::function<bool(std::uint64_t)> cancel_timer;
};

enum class DispatchStatus { NotFound, MethodNotAllowed, Complete, Pending };

struct DispatchResult {
    DispatchStatus status = DispatchStatus::NotFound;
    HttpResponse response;
};

// 路由表只在启动阶段写入，运行期只读；因此多个 IO worker 可以并发 dispatch。
class Router : private NonCopyable {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    using AsyncHandler = std::function<void(HttpRequest&, const AsyncContext&)>;

    void add(std::string method, std::string pattern, Handler handler);
    void addAsync(std::string method, std::string pattern, AsyncHandler handler);

    DispatchResult dispatch(HttpRequest& request, const AsyncContext& context) const;
    std::optional<HttpResponse> dispatch(HttpRequest& request) const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
        AsyncHandler async_handler;
    };

    static std::vector<std::string> splitPath(const std::string& path);
    static bool matchSegments(const std::vector<std::string>& pattern,
                              const std::vector<std::string>& actual,
                              HttpRequest* request);

    std::vector<Route> routes_;
};

}  // namespace http
}  // namespace cofinance
