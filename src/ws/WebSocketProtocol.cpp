#include "cofinance/ws/WebSocketProtocol.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace cofinance {
namespace ws {
namespace {

ParseResult fail(std::uint16_t code, const char* message, std::uint16_t* close_code,
                 std::string* error) {
    if (close_code != nullptr) {
        *close_code = code;
    }
    if (error != nullptr) {
        *error = message;
    }
    return ParseResult::Error;
}

std::uint16_t readUint16(const char* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[0]) << 8) |
                                      static_cast<std::uint8_t>(data[1]));
}

std::uint64_t readUint64(const char* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(data[i]);
    }
    return value;
}

void appendUint64(std::string* out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

}  // namespace

FrameParser::FrameParser(std::size_t max_payload_bytes)
    : max_payload_bytes_(max_payload_bytes == 0 ? 1 : max_payload_bytes) {}

ParseResult FrameParser::parse(std::string& buffer, Frame* frame, std::uint16_t* close_code,
                               std::string* error) const {
    if (frame == nullptr) {
        return fail(1002, "帧输出参数为空", close_code, error);
    }
    if (buffer.size() < 2) {
        return ParseResult::NeedMore;
    }

    const auto first = static_cast<std::uint8_t>(buffer[0]);
    const auto second = static_cast<std::uint8_t>(buffer[1]);
    const bool fin = (first & 0x80U) != 0;
    const std::uint8_t opcode_value = first & 0x0fU;
    const auto opcode = static_cast<Opcode>(opcode_value);
    if ((first & 0x70U) != 0) {
        return fail(1002, "RSV 位必须为 0", close_code, error);
    }
    if (!isValidOpcode(opcode)) {
        return fail(1002, "未知 WebSocket opcode", close_code, error);
    }
    const bool masked = (second & 0x80U) != 0;
    if (!masked) {
        return fail(1002, "客户端帧必须使用掩码", close_code, error);
    }

    const std::uint8_t length_code = second & 0x7fU;
    if (isControlOpcode(opcode) && (!fin || length_code > 125)) {
        return fail(1002, "控制帧不能分片且长度不能超过 125", close_code, error);
    }

    std::size_t offset = 2;
    std::uint64_t payload_length = length_code;
    if (length_code == 126) {
        if (buffer.size() < offset + 2) {
            return ParseResult::NeedMore;
        }
        payload_length = readUint16(buffer.data() + offset);
        offset += 2;
    } else if (length_code == 127) {
        if (buffer.size() < offset + 8) {
            return ParseResult::NeedMore;
        }
        payload_length = readUint64(buffer.data() + offset);
        offset += 8;
        if ((payload_length & (std::uint64_t{1} << 63)) != 0) {
            return fail(1002, "WebSocket 长度最高位必须为 0", close_code, error);
        }
    }
    if (payload_length > max_payload_bytes_) {
        return fail(1009, "WebSocket 消息超过上限", close_code, error);
    }

    if (buffer.size() < offset + 4) {
        return ParseResult::NeedMore;
    }
    const char mask[4] = {buffer[offset], buffer[offset + 1], buffer[offset + 2],
                          buffer[offset + 3]};
    offset += 4;

    if (payload_length > std::numeric_limits<std::size_t>::max() - offset) {
        return fail(1009, "WebSocket 帧长度溢出", close_code, error);
    }
    const std::size_t total = offset + static_cast<std::size_t>(payload_length);
    if (buffer.size() < total) {
        return ParseResult::NeedMore;
    }

    std::string payload = buffer.substr(offset, static_cast<std::size_t>(payload_length));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                       static_cast<std::uint8_t>(mask[i % 4]));
    }
    buffer.erase(0, total);

    frame->fin = fin;
    frame->opcode = opcode;
    frame->payload = std::move(payload);
    if (close_code != nullptr) {
        *close_code = 0;
    }
    if (error != nullptr) {
        error->clear();
    }
    return ParseResult::Frame;
}

bool isControlOpcode(Opcode opcode) noexcept {
    return opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
}

bool isValidOpcode(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Continuation:
        case Opcode::Text:
        case Opcode::Binary:
        case Opcode::Close:
        case Opcode::Ping:
        case Opcode::Pong:
            return true;
        default:
            return false;
    }
}

bool isValidCloseCode(std::uint16_t code) noexcept {
    if (code >= 3000 && code <= 4999) {
        return true;
    }
    if (code < 1000 || code > 1014) {
        return false;
    }
    return code != 1004 && code != 1005 && code != 1006;
}

std::string encodeFrame(Opcode opcode, std::string_view payload, bool fin) {
    std::string out;
    out.reserve(payload.size() + 14);
    out.push_back(static_cast<char>((fin ? 0x80U : 0U) | static_cast<std::uint8_t>(opcode)));
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffffU) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xffU));
        out.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
        out.push_back(static_cast<char>(127));
        appendUint64(&out, static_cast<std::uint64_t>(payload.size()));
    }
    out.append(payload.data(), payload.size());
    return out;
}

std::string encodeCloseFrame(std::uint16_t code, std::string_view reason) {
    std::string payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<char>((code >> 8) & 0xffU));
    payload.push_back(static_cast<char>(code & 0xffU));
    const std::size_t max_reason = 123;
    payload.append(reason.substr(0, std::min(reason.size(), max_reason)));
    return encodeFrame(Opcode::Close, payload, true);
}

bool parseClosePayload(std::string_view payload, std::uint16_t* code, std::string* reason) {
    if (code == nullptr || reason == nullptr || payload.size() == 1) {
        return false;
    }
    if (payload.empty()) {
        *code = 1000;
        reason->clear();
        return true;
    }
    const std::uint16_t parsed = static_cast<std::uint16_t>(
        (static_cast<std::uint8_t>(payload[0]) << 8) |
        static_cast<std::uint8_t>(payload[1]));
    if (!isValidCloseCode(parsed)) {
        return false;
    }
    std::string parsed_reason(payload.substr(2));
    bool valid_utf8 = false;
    util::utf8Length(parsed_reason, &valid_utf8);
    if (!valid_utf8) {
        return false;
    }
    *code = parsed;
    *reason = std::move(parsed_reason);
    return true;
}

}  // namespace ws
}  // namespace cofinance
