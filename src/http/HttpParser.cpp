#include "cofinance/http/HttpParser.h"

#include <cctype>
#include <string>
#include <utility>

namespace cofinance {
namespace http {
namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    }
    return -1;
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

}  // namespace

HttpParser::HttpParser(std::size_t header_limit, std::size_t body_limit)
    : header_limit_(header_limit), body_limit_(body_limit) {}

void HttpParser::reset() {
    state_ = State::RequestLine;
    request_ = HttpRequest {};
    header_bytes_ = 0;
    header_count_ = 0;
    content_length_ = 0;
    saw_content_length_ = false;
    saw_host_ = false;
    saw_transfer_encoding_ = false;
    saw_expect_ = false;
}

HttpParser::Result HttpParser::needMore() {
    return Result::NeedMore;
}

bool HttpParser::fail(int status, std::string message, HttpParseError* error) {
    state_ = State::Error;
    if (error != nullptr) {
        error->status = status;
        error->message = std::move(message);
    }
    return false;
}

HttpParser::Result HttpParser::parse(std::string& buffer, HttpRequest* request,
                                     HttpParseError* error) {
    if (state_ == State::Error) {
        return Result::Error;
    }
    if (state_ == State::Complete) {
        return Result::Complete;
    }

    while (state_ == State::RequestLine || state_ == State::Headers) {
        const std::size_t line_end = buffer.find("\r\n");
        if (line_end == std::string::npos) {
            if (header_bytes_ + buffer.size() > header_limit_) {
                fail(431, "request headers exceed limit", error);
                return Result::Error;
            }
            return needMore();
        }
        if (header_bytes_ + line_end + 2 > header_limit_) {
            fail(431, "request headers exceed limit", error);
            return Result::Error;
        }

        std::string line = buffer.substr(0, line_end);
        buffer.erase(0, line_end + 2);
        header_bytes_ += line_end + 2;

        if (state_ == State::RequestLine) {
            if (!parseRequestLine(line, error)) {
                return Result::Error;
            }
            state_ = State::Headers;
            continue;
        }

        if (line.empty()) {
            if (!finishHeaders(error)) {
                return Result::Error;
            }
            break;
        }
        if (++header_count_ > 100) {
            fail(431, "too many request headers", error);
            return Result::Error;
        }
        if (!parseHeaderLine(line, error)) {
            return Result::Error;
        }
    }

    if (state_ == State::Complete) {
        if (request != nullptr) {
            *request = std::move(request_);
        }
        return Result::Complete;
    }
    if (state_ != State::Body) {
        return Result::Error;
    }
    if (buffer.size() < content_length_) {
        return needMore();
    }

    request_.body.assign(buffer.data(), content_length_);
    buffer.erase(0, content_length_);
    state_ = State::Complete;
    if (request != nullptr) {
        *request = std::move(request_);
    }
    return Result::Complete;
}

bool HttpParser::parseRequestLine(const std::string& line, HttpParseError* error) {
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        return fail(400, "invalid request line", error);
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos || second_space + 1 >= line.size() ||
        line.find(' ', second_space + 1) != std::string::npos) {
        return fail(400, "invalid request line", error);
    }

    request_.method = line.substr(0, first_space);
    request_.target = line.substr(first_space + 1, second_space - first_space - 1);
    request_.version = line.substr(second_space + 1);
    if (request_.method.empty() || request_.method.size() > 32) {
        return fail(400, "invalid method", error);
    }
    for (char c : request_.method) {
        if (!isTokenChar(c)) {
            return fail(400, "invalid method", error);
        }
    }
    if (request_.version != "HTTP/1.1" && request_.version != "HTTP/1.0") {
        return fail(505, "unsupported HTTP version", error);
    }
    if (request_.target.empty() || request_.target.front() != '/' ||
        request_.target.find('#') != std::string::npos) {
        return fail(400, "invalid request target", error);
    }
    for (char c : request_.target) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7f) {
            return fail(400, "invalid request target", error);
        }
    }

    const std::size_t query_pos = request_.target.find('?');
    const std::string raw_path = request_.target.substr(0, query_pos);
    request_.query = query_pos == std::string::npos ? std::string()
                                                     : request_.target.substr(query_pos + 1);
    if (!percentDecodePath(raw_path, &request_.path)) {
        return fail(400, "invalid percent encoding", error);
    }
    request_.keep_alive = request_.version == "HTTP/1.1";
    return true;
}

bool HttpParser::parseHeaderLine(const std::string& line, HttpParseError* error) {
    if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        return fail(400, "obsolete header folding is not supported", error);
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
        return fail(400, "invalid header line", error);
    }

    const std::string raw_name = line.substr(0, colon);
    for (char c : raw_name) {
        if (!isTokenChar(c)) {
            return fail(400, "invalid header name", error);
        }
    }
    const std::string name = lowerAscii(raw_name);
    const std::string value = util::trim(line.substr(colon + 1));
    if (!validHeaderValue(value)) {
        return fail(400, "invalid header value", error);
    }

    if (name == "content-length") {
        if (saw_content_length_) {
            return fail(400, "duplicate Content-Length", error);
        }
        if (value.empty()) {
            return fail(400, "invalid Content-Length", error);
        }
        std::size_t length = 0;
        for (char c : value) {
            if (c < '0' || c > '9') {
                return fail(400, "invalid Content-Length", error);
            }
            const std::size_t digit = static_cast<std::size_t>(c - '0');
            if (length > (body_limit_ - digit) / 10) {
                return fail(413, "request body too large", error);
            }
            length = length * 10 + digit;
        }
        content_length_ = length;
        saw_content_length_ = true;
    } else if (name == "transfer-encoding") {
        if (saw_transfer_encoding_) {
            return fail(400, "duplicate Transfer-Encoding", error);
        }
        saw_transfer_encoding_ = true;
        return fail(501, "Transfer-Encoding is not supported", error);
    } else if (name == "host") {
        if (saw_host_) {
            return fail(400, "duplicate Host", error);
        }
        saw_host_ = true;
    } else if (name == "expect") {
        saw_expect_ = true;
        return fail(417, "Expect header is not supported", error);
    } else if (name == "connection") {
        std::size_t begin = 0;
        while (begin <= value.size()) {
            const std::size_t comma = value.find(',', begin);
            const std::string token = lowerAscii(
                util::trim(value.substr(begin, comma == std::string::npos ? std::string::npos
                                                                          : comma - begin)));
            if (token == "close") {
                request_.keep_alive = false;
            } else if (token == "keep-alive") {
                request_.keep_alive = true;
            }
            if (comma == std::string::npos) {
                break;
            }
            begin = comma + 1;
        }
    }

    const auto inserted = request_.headers.emplace(name, value);
    if (!inserted.second) {
        inserted.first->second += ", ";
        inserted.first->second += value;
    }
    return true;
}

bool HttpParser::finishHeaders(HttpParseError* error) {
    if (request_.version == "HTTP/1.1" && !saw_host_) {
        return fail(400, "HTTP/1.1 requires Host", error);
    }
    if (saw_expect_) {
        return fail(417, "Expect header is not supported", error);
    }
    if (content_length_ > body_limit_) {
        return fail(413, "request body too large", error);
    }
    state_ = content_length_ == 0 ? State::Complete : State::Body;
    return true;
}

bool HttpParser::isTokenChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool HttpParser::validHeaderValue(const std::string& value) {
    for (char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\t') {
            continue;
        }
        if (uc < 0x20 || uc == 0x7f) {
            return false;
        }
    }
    return true;
}

bool HttpParser::percentDecodePath(const std::string& raw, std::string* decoded) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '%') {
            if (i + 2 >= raw.size()) {
                return false;
            }
            const int high = hexValue(raw[i + 1]);
            const int low = hexValue(raw[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            c = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        }
        if (c == 0 || c < 0x20 || c == 0x7f) {
            return false;
        }
        out.push_back(static_cast<char>(c));
    }
    if (decoded != nullptr) {
        *decoded = std::move(out);
    }
    return true;
}

}  // namespace http
}  // namespace cofinance
