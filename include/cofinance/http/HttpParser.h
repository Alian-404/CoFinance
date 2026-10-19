#pragma once

#include <cstddef>
#include <string>

#include "cofinance/http/HttpTypes.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace http {

struct HttpParseError {
    int status = 400;
    std::string message;
};

// 增量解析 HTTP/1.x 请求。调用方持有 TcpConnection 的读缓冲；每次 parse 只消费
// 已经确认完整的请求行、头和 body，剩余字节留给下一个请求（pipeline）。
class HttpParser : private NonCopyable {
public:
    enum class Result { NeedMore, Complete, Error };

    HttpParser(std::size_t header_limit, std::size_t body_limit);

    Result parse(std::string& buffer, HttpRequest* request, HttpParseError* error);
    void reset();

private:
    enum class State { RequestLine, Headers, Body, Complete, Error };

    Result needMore();
    bool fail(int status, std::string message, HttpParseError* error);
    bool parseRequestLine(const std::string& line, HttpParseError* error);
    bool parseHeaderLine(const std::string& line, HttpParseError* error);
    bool finishHeaders(HttpParseError* error);
    static bool isTokenChar(char c);
    static bool validHeaderValue(const std::string& value);
    static bool percentDecodePath(const std::string& raw, std::string* decoded);

    std::size_t header_limit_;
    std::size_t body_limit_;
    State state_ = State::RequestLine;
    HttpRequest request_;
    std::size_t header_bytes_ = 0;
    std::size_t header_count_ = 0;
    std::size_t content_length_ = 0;
    bool saw_content_length_ = false;
    bool saw_host_ = false;
    bool saw_transfer_encoding_ = false;
    bool saw_expect_ = false;
};

}  // namespace http
}  // namespace cofinance
