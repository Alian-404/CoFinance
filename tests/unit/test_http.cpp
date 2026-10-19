#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cofinance/http/HttpParser.h"
#include "cofinance/http/Router.h"
#include "cofinance/http/StaticFileHandler.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                            \
    } while (false)

void testCompleteRequest() {
    std::printf("[case] 完整请求与 query\n");
    cofinance::http::HttpParser parser(8192, 1024 * 1024);
    std::string buffer =
        "GET /api/ping?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;
    CHECK(parser.parse(buffer, &request, &error) == cofinance::http::HttpParser::Result::Complete);
    CHECK(buffer.empty());
    CHECK(request.method == "GET");
    CHECK(request.path == "/api/ping");
    CHECK(request.query == "x=1");
    CHECK(request.version == "HTTP/1.1");
    CHECK(request.keep_alive);
    CHECK(request.header("Host") == "localhost");
}

void testHalfPacket() {
    std::printf("[case] 半包：逐段喂入\n");
    cofinance::http::HttpParser parser(8192, 1024 * 1024);
    const std::string pieces[] = {"GET /api/pi", "ng HTTP/1.1\r\nHo", "st: local",
                                  "host\r\n\r", "\n"};
    std::string buffer;
    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;
    bool complete = false;
    for (const std::string& piece : pieces) {
        buffer += piece;
        const auto result = parser.parse(buffer, &request, &error);
        if (result == cofinance::http::HttpParser::Result::Complete) {
            complete = true;
        }
    }
    CHECK(complete);
    CHECK(request.path == "/api/ping");
    CHECK(buffer.empty());
}

void testBodyAndPipeline() {
    std::printf("[case] body 与 pipelining\n");
    cofinance::http::HttpParser parser(8192, 1024 * 1024);
    std::string first =
        "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";
    const std::string second =
        "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    first += second;

    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;
    CHECK(parser.parse(first, &request, &error) == cofinance::http::HttpParser::Result::Complete);
    CHECK(request.body == "hello");
    CHECK(request.keep_alive);

    parser.reset();
    cofinance::http::HttpRequest next;
    CHECK(parser.parse(first, &next, &error) == cofinance::http::HttpParser::Result::Complete);
    CHECK(next.path == "/api/ping");
    CHECK(!next.keep_alive);
    CHECK(first.empty());
}

void testSplitBody() {
    std::printf("[case] body 跨包到达\n");
    cofinance::http::HttpParser parser(8192, 1024);
    std::string buffer =
        "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nhello";
    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;
    CHECK(parser.parse(buffer, &request, &error) ==
          cofinance::http::HttpParser::Result::NeedMore);
    buffer += "world";
    CHECK(parser.parse(buffer, &request, &error) ==
          cofinance::http::HttpParser::Result::Complete);
    CHECK(request.body == "helloworld");
    CHECK(buffer.empty());
}

void testParserErrors() {
    std::printf("[case] 协议错误与限制\n");
    cofinance::http::HttpParser parser(128, 1024);
    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;

    std::string missing_host = "GET / HTTP/1.1\r\n\r\n";
    CHECK(parser.parse(missing_host, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 400);

    parser.reset();
    std::string duplicate_cl =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na";
    CHECK(parser.parse(duplicate_cl, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 400);

    parser.reset();
    std::string too_large =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000\r\n\r\n";
    CHECK(parser.parse(too_large, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 413);

    parser.reset();
    std::string transfer_encoding =
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(parser.parse(transfer_encoding, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 501);

    parser.reset();
    std::string huge_header = "GET / HTTP/1.1\r\nHost: x\r\nX-Long: ";
    huge_header.append(256, 'a');
    CHECK(parser.parse(huge_header, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 431);

    parser.reset();
    std::string bad_percent = "GET /a%zz HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(parser.parse(bad_percent, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 400);

    parser.reset();
    std::string bad_version = "GET / HTTP/2.0\r\nHost: x\r\n\r\n";
    CHECK(parser.parse(bad_version, &request, &error) ==
          cofinance::http::HttpParser::Result::Error);
    CHECK(error.status == 505);
}

void testHttp10KeepAlive() {
    std::printf("[case] HTTP/1.0 keep-alive 语义\n");
    cofinance::http::HttpParser parser(8192, 1024);
    cofinance::http::HttpRequest request;
    cofinance::http::HttpParseError error;
    std::string close_default = "GET / HTTP/1.0\r\n\r\n";
    CHECK(parser.parse(close_default, &request, &error) ==
          cofinance::http::HttpParser::Result::Complete);
    CHECK(!request.keep_alive);

    parser.reset();
    std::string keep = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    CHECK(parser.parse(keep, &request, &error) ==
          cofinance::http::HttpParser::Result::Complete);
    CHECK(request.keep_alive);
}

void testRouter() {
    std::printf("[case] 路由与路径参数\n");
    cofinance::http::Router router;
    router.add("GET", "/api/items/{id}", [](const cofinance::http::HttpRequest& request) {
        return cofinance::http::HttpResponse::text(200, request.path_params.at("id"));
    });

    cofinance::http::HttpRequest request;
    request.method = "GET";
    request.path = "/api/items/42";
    auto response = router.dispatch(request);
    CHECK(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == "42");

    request.method = "POST";
    response = router.dispatch(request);
    CHECK(response.has_value());
    CHECK(response->status == 405);

    request.method = "GET";
    request.path = "/not-found";
    response = router.dispatch(request);
    CHECK(!response.has_value());
}

void testStaticFiles() {
    std::printf("[case] 静态文件与路径穿越\n");
    const std::filesystem::path root = "/tmp/cofinance_p2_www";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    {
        std::ofstream out(root / "index.html", std::ios::binary);
        out << "<h1>CoFinance</h1>";
    }

    cofinance::http::StaticFileHandler handler(root.string());
    std::string err;
    CHECK(handler.init(&err));

    cofinance::http::HttpRequest request;
    request.method = "GET";
    request.path = "/";
    cofinance::http::HttpResponse response = handler.handle(request);
    CHECK(response.status == 200);
    CHECK(response.body.find("CoFinance") != std::string::npos);
    CHECK(response.content_type.find("text/html") == 0);

    request.path = "/../etc/passwd";
    response = handler.handle(request);
    CHECK(response.status == 404);

    request.path = "/missing.txt";
    response = handler.handle(request);
    CHECK(response.status == 404);
    std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
    std::printf("=== CoFinance HTTP 单元测试（P2）===\n");
    testCompleteRequest();
    testHalfPacket();
    testBodyAndPipeline();
    testSplitBody();
    testParserErrors();
    testHttp10KeepAlive();
    testRouter();
    testStaticFiles();
    std::printf("=== 检查点 %d 个, 失败 %d 个 ===\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
