#include "cofinance/ws/WebSocketHandshake.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstddef>
#include <string>
#include <vector>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace ws {
namespace {

constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool containsToken(const std::string& value, const std::string& expected) {
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t comma = value.find(',', begin);
        const std::string token = util::trim(value.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (util::iequals(token, expected)) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return false;
}

bool validClientKey(const std::string& key) {
    if (key.empty() || key.size() > 256 || key.size() % 4 != 0) {
        return false;
    }
    std::vector<unsigned char> decoded(key.size());
    const int length = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(key.data()),
                                       static_cast<int>(key.size()));
    if (length < 0) {
        return false;
    }
    std::size_t padding = 0;
    if (!key.empty() && key.back() == '=') {
        ++padding;
    }
    if (key.size() >= 2 && key[key.size() - 2] == '=') {
        ++padding;
    }
    return static_cast<std::size_t>(length) >= padding &&
           static_cast<std::size_t>(length) - padding == 16;
}

std::string base64Encode(const unsigned char* data, std::size_t size) {
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                                        static_cast<int>(size));
    if (written < 0) {
        return std::string();
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

}  // namespace

bool validateHandshake(const http::HttpRequest& request, std::string* accept, int* status,
                       std::string* error) {
    if (status != nullptr) {
        *status = 400;
    }
    if (request.method != "GET" || request.version != "HTTP/1.1" || !request.body.empty()) {
        if (error != nullptr) {
            *error = "WebSocket 握手必须是无 body 的 HTTP/1.1 GET";
        }
        return false;
    }
    if (!containsToken(request.header("upgrade"), "websocket") ||
        !containsToken(request.header("connection"), "upgrade")) {
        if (error != nullptr) {
            *error = "缺少 Upgrade: websocket 或 Connection: Upgrade";
        }
        return false;
    }
    if (util::trim(request.header("sec-websocket-version")) != "13") {
        if (status != nullptr) {
            *status = 426;
        }
        if (error != nullptr) {
            *error = "只支持 WebSocket 版本 13";
        }
        return false;
    }
    const std::string key = util::trim(request.header("sec-websocket-key"));
    if (!validClientKey(key)) {
        if (error != nullptr) {
            *error = "Sec-WebSocket-Key 必须是 16 字节的 Base64";
        }
        return false;
    }
    const std::string computed = handshakeAcceptKey(key);
    if (computed.empty()) {
        if (status != nullptr) {
            *status = 500;
        }
        if (error != nullptr) {
            *error = "计算 Sec-WebSocket-Accept 失败";
        }
        return false;
    }
    if (accept != nullptr) {
        *accept = computed;
    }
    if (status != nullptr) {
        *status = 101;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::string handshakeAcceptKey(const std::string& client_key) {
    const std::string source = client_key + kWebSocketGuid;
    unsigned char digest[SHA_DIGEST_LENGTH];
    if (SHA1(reinterpret_cast<const unsigned char*>(source.data()), source.size(), digest) ==
        nullptr) {
        return std::string();
    }
    return base64Encode(digest, sizeof(digest));
}

std::string handshakeResponse(const std::string& accept) {
    std::string response;
    response.reserve(180);
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: ";
    response += accept;
    response += "\r\n\r\n";
    return response;
}

}  // namespace ws
}  // namespace cofinance
