#pragma once

#include <string>

#include "cofinance/http/HttpTypes.h"

namespace cofinance {
namespace ws {

// 校验 RFC 6455 升级请求并计算 Sec-WebSocket-Accept。
// 成功返回 true；失败时 status 给出应回复的 HTTP 状态码。
bool validateHandshake(const http::HttpRequest& request, std::string* accept, int* status,
                       std::string* error);

std::string handshakeAcceptKey(const std::string& client_key);
std::string handshakeResponse(const std::string& accept);

}  // namespace ws
}  // namespace cofinance
