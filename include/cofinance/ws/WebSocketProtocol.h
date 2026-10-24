#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    bool fin = true;
    Opcode opcode = Opcode::Text;
    std::string payload;
};

enum class ParseResult { NeedMore, Frame, Error };

// 解析客户端到服务端的帧。RFC 6455 要求客户端帧必须带掩码；解析成功后
// 只消费一个完整帧，剩余字节留给下一次 parse（处理粘包/半包）。
class FrameParser : private NonCopyable {
public:
    explicit FrameParser(std::size_t max_payload_bytes = 64 * 1024);

    ParseResult parse(std::string& buffer, Frame* frame, std::uint16_t* close_code,
                      std::string* error) const;

private:
    std::size_t max_payload_bytes_;
};

bool isControlOpcode(Opcode opcode) noexcept;
bool isValidOpcode(Opcode opcode) noexcept;
bool isValidCloseCode(std::uint16_t code) noexcept;

std::string encodeFrame(Opcode opcode, std::string_view payload, bool fin = true);
std::string encodeCloseFrame(std::uint16_t code, std::string_view reason);
bool parseClosePayload(std::string_view payload, std::uint16_t* code, std::string* reason);

}  // namespace ws
}  // namespace cofinance
