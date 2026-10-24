#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

#include "cofinance/db/MessageDao.h"
#include "cofinance/net/EventLoop.h"
#include "cofinance/net/TcpConnection.h"
#include "cofinance/service/RealtimeRegistry.h"
#include "cofinance/util/Common.h"
#include "cofinance/ws/WebSocketHandshake.h"
#include "cofinance/ws/WebSocketProtocol.h"
#include "cofinance/ws/WebSocketSession.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                            \
    } while (false)

std::string clientFrame(std::uint8_t opcode, const std::string& payload, bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80U : 0U) | opcode));
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(0x80U | payload.size()));
    } else if (payload.size() <= 0xffffU) {
        out.push_back(static_cast<char>(0x80U | 126U));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xffU));
        out.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
        out.push_back(static_cast<char>(0x80U | 127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((payload.size() >> shift) & 0xffU));
        }
    }
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    out.append(mask, 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                        static_cast<std::uint8_t>(mask[i % 4])));
    }
    return out;
}

cofinance::http::HttpRequest handshakeRequest() {
    cofinance::http::HttpRequest request;
    request.method = "GET";
    request.path = "/api/ws";
    request.version = "HTTP/1.1";
    request.headers["host"] = "localhost";
    request.headers["upgrade"] = "WebSocket";
    request.headers["connection"] = "keep-alive, Upgrade";
    request.headers["sec-websocket-version"] = "13";
    request.headers["sec-websocket-key"] = "dGhlIHNhbXBsZSBub25jZQ==";
    return request;
}

void testHandshake() {
    std::printf("[case] WebSocket 握手\n");
    CHECK(cofinance::ws::handshakeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    cofinance::http::HttpRequest request = handshakeRequest();
    std::string accept;
    int status = 0;
    std::string error;
    CHECK(cofinance::ws::validateHandshake(request, &accept, &status, &error));
    CHECK(status == 101);
    CHECK(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(error.empty());
    CHECK(cofinance::ws::handshakeResponse(accept).find("101 Switching Protocols") !=
          std::string::npos);

    request = handshakeRequest();
    request.headers["sec-websocket-version"] = "12";
    CHECK(!cofinance::ws::validateHandshake(request, &accept, &status, &error));
    CHECK(status == 426);

    request = handshakeRequest();
    request.headers["upgrade"].clear();
    CHECK(!cofinance::ws::validateHandshake(request, &accept, &status, &error));
    CHECK(status == 400);

    request = handshakeRequest();
    request.headers["sec-websocket-key"] = "not-base64";
    CHECK(!cofinance::ws::validateHandshake(request, &accept, &status, &error));
    CHECK(status == 400);

    request = handshakeRequest();
    request.body = "x";
    CHECK(!cofinance::ws::validateHandshake(request, &accept, &status, &error));
    CHECK(status == 400);
}

void testFrameParser() {
    std::printf("[case] WebSocket 帧解析\n");
    cofinance::ws::FrameParser parser(100000);
    cofinance::ws::Frame frame;
    std::uint16_t close_code = 0;
    std::string error;

    std::string buffer = clientFrame(0x1, "hello");
    buffer += clientFrame(0x9, "p");
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Frame);
    CHECK(frame.fin);
    CHECK(frame.opcode == cofinance::ws::Opcode::Text);
    CHECK(frame.payload == "hello");
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Frame);
    CHECK(frame.opcode == cofinance::ws::Opcode::Ping);
    CHECK(frame.payload == "p");
    CHECK(buffer.empty());

    const std::string split_frame = clientFrame(0x1, "partial");
    buffer = split_frame.substr(0, 4);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::NeedMore);
    buffer += split_frame.substr(4);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Frame);
    CHECK(frame.payload == "partial");

    const std::string long_payload(300, 'a');
    buffer = clientFrame(0x1, long_payload);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Frame);
    CHECK(frame.payload == long_payload);

    const std::string huge_payload(70000, 'b');
    buffer = clientFrame(0x2, huge_payload);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Frame);
    CHECK(frame.payload.size() == huge_payload.size());

    buffer = clientFrame(0x1, "x");
    buffer[1] = static_cast<char>(buffer[1] & 0x7f);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Error);
    CHECK(close_code == 1002);

    buffer = clientFrame(0x1, "x");
    buffer[0] = static_cast<char>(buffer[0] | 0x40);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Error);
    CHECK(close_code == 1002);

    buffer = clientFrame(0x9, "x", false);
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Error);
    CHECK(close_code == 1002);

    buffer = clientFrame(0x3, "x");
    CHECK(parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Error);
    CHECK(close_code == 1002);

    cofinance::ws::FrameParser small_parser(8);
    buffer = clientFrame(0x1, "123456789");
    CHECK(small_parser.parse(buffer, &frame, &close_code, &error) ==
          cofinance::ws::ParseResult::Error);
    CHECK(close_code == 1009);
}

void testFrameEncodingAndClose() {
    std::printf("[case] WebSocket 编码与关闭帧\n");
    const std::string short_frame = cofinance::ws::encodeFrame(
        cofinance::ws::Opcode::Text, "hello");
    CHECK(short_frame.size() == 7);
    CHECK(static_cast<std::uint8_t>(short_frame[0]) == 0x81U);
    CHECK(static_cast<std::uint8_t>(short_frame[1]) == 5U);

    const std::string medium = cofinance::ws::encodeFrame(
        cofinance::ws::Opcode::Text, std::string(300, 'x'));
    CHECK(static_cast<std::uint8_t>(medium[1]) == 126U);

    const std::string long_frame = cofinance::ws::encodeFrame(
        cofinance::ws::Opcode::Text, std::string(70000, 'x'));
    CHECK(static_cast<std::uint8_t>(long_frame[1]) == 127U);

    std::uint16_t code = 0;
    std::string reason;
    CHECK(cofinance::ws::parseClosePayload(std::string("\x03\xe8""bye", 5), &code, &reason));
    CHECK(code == 1000);
    CHECK(reason == "bye");
    CHECK(!cofinance::ws::parseClosePayload("x", &code, &reason));
    CHECK(!cofinance::ws::parseClosePayload(std::string("\x03\xed", 2), &code, &reason));
    CHECK(!cofinance::ws::parseClosePayload(std::string("\x03\xe8\xff", 3), &code, &reason));
}

struct SocketPair {
    int peer = -1;
    int owned = -1;

    bool open() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            return false;
        }
        peer = fds[0];
        owned = fds[1];
        const int flags = ::fcntl(owned, F_GETFL, 0);
        return flags >= 0 && ::fcntl(owned, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    ~SocketPair() {
        if (peer >= 0) {
            ::close(peer);
        }
        if (owned >= 0) {
            ::close(owned);
        }
    }
};

bool readServerFrame(int fd, cofinance::ws::Opcode* opcode, std::string* payload) {
    unsigned char header[2] {};
    ssize_t n = ::recv(fd, header, 2, MSG_WAITALL);
    if (n != 2) {
        return false;
    }
    *opcode = static_cast<cofinance::ws::Opcode>(header[0] & 0x0fU);
    std::uint64_t length = header[1] & 0x7fU;
    if (length == 126) {
        unsigned char ext[2] {};
        if (::recv(fd, ext, 2, MSG_WAITALL) != 2) {
            return false;
        }
        length = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (length == 127) {
        unsigned char ext[8] {};
        if (::recv(fd, ext, 8, MSG_WAITALL) != 8) {
            return false;
        }
        length = 0;
        for (unsigned char byte : ext) {
            length = (length << 8) | byte;
        }
    }
    payload->assign(static_cast<std::size_t>(length), '\0');
    return length == 0 || ::recv(fd, payload->data(), payload->size(), MSG_WAITALL) ==
                              static_cast<ssize_t>(length);
}

void testRealtimeRegistry() {
    std::printf("[case] 实时订阅表、限额与注销\n");
    cofinance::service::RealtimeLimits limits;
    limits.per_user = 2;
    limits.per_group = 3;
    limits.total = 4;
    cofinance::service::RealtimeRegistry registry(limits);
    int delivered = 0;
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    auto callback = [&delivered](const cofinance::db::MessageRecord& message) {
        if (message.id != 0) {
            ++delivered;
        }
    };
    CHECK(registry.add(10, 100, callback, &first) ==
          cofinance::service::RealtimeRegisterResult::Ok);
    CHECK(registry.add(10, 100, callback, &second) ==
          cofinance::service::RealtimeRegisterResult::Ok);
    std::uint64_t third = 0;
    CHECK(registry.add(10, 100, callback, &third) ==
          cofinance::service::RealtimeRegisterResult::UserLimit);
    CHECK(registry.add(10, 101, callback, &third) ==
          cofinance::service::RealtimeRegisterResult::Ok);
    CHECK(registry.add(10, 102, callback, &third) ==
          cofinance::service::RealtimeRegisterResult::GroupLimit);
    CHECK(registry.add(11, 102, callback, &third) ==
          cofinance::service::RealtimeRegisterResult::Ok);
    std::uint64_t overflow = 0;
    CHECK(registry.add(11, 103, callback, &overflow) ==
          cofinance::service::RealtimeRegisterResult::TotalLimit);
    CHECK(registry.size() == 4);
    CHECK(registry.countForUser(100) == 2);
    CHECK(registry.countForGroup(10) == 3);

    cofinance::db::MessageRecord message;
    message.id = 7;
    message.group_id = 10;
    registry.notify(10, message);
    CHECK(delivered == 3);
    registry.removeForUser(10, 100);
    CHECK(registry.countForUser(100) == 0);
    CHECK(registry.countForGroup(10) == 1);
    registry.removeForGroup(10);
    CHECK(registry.countForGroup(10) == 0);
    CHECK(registry.size() == 1);
}

void testSessionFramesAndHeartbeat() {
    std::printf("[case] WebSocket 会话分片、Ping/Pong 与心跳\n");
    SocketPair sockets;
    CHECK(sockets.open());
    cofinance::net::EventLoop loop;
    std::string err;
    CHECK(loop.init(&err));
    auto connection = std::make_shared<cofinance::net::TcpConnection>(
        &loop, sockets.owned, "socketpair", 0);
    sockets.owned = -1;
    CHECK(connection->start(&err));

    cofinance::ws::WebSocketSession::Config config;
    config.max_message_bytes = 1024;
    config.heartbeat_interval_ms = 20;
    config.idle_timeout_ms = 60;
    auto session = std::make_shared<cofinance::ws::WebSocketSession>(connection, config);
    std::string received;
    std::uint16_t close_code = 0;
    session->setTextCallback([&received](const std::string& text) { received = text; });
    session->setCloseCallback(
        [&close_code](std::uint16_t code, const std::string&) { close_code = code; });
    std::string empty;
    session->start(empty);

    std::string fragments = clientFrame(0x1, "hel", false);
    fragments += clientFrame(0x0, "lo", true);
    session->onData(fragments);
    CHECK(received == "hello");

    std::string ping = clientFrame(0x9, "p");
    session->onData(ping);
    CHECK(!connection->closed());
    loop.pollOnce(100);
    CHECK(!connection->closed());
    cofinance::ws::Opcode opcode = cofinance::ws::Opcode::Close;
    std::string payload;
    CHECK(readServerFrame(sockets.peer, &opcode, &payload));
    CHECK(opcode == cofinance::ws::Opcode::Pong);
    CHECK(payload == "p");

    const std::int64_t deadline = cofinance::util::nowMonotonicMillis() + 1000;
    while (close_code == 0 && cofinance::util::nowMonotonicMillis() < deadline) {
        loop.pollOnce(20);
    }
    CHECK(close_code == 1001);
    CHECK(session->closed());
}

void testSessionRejectsBinary() {
    std::printf("[case] WebSocket 会话拒绝二进制消息\n");
    SocketPair sockets;
    CHECK(sockets.open());
    cofinance::net::EventLoop loop;
    std::string err;
    CHECK(loop.init(&err));
    auto connection = std::make_shared<cofinance::net::TcpConnection>(
        &loop, sockets.owned, "socketpair", 0);
    sockets.owned = -1;
    CHECK(connection->start(&err));
    cofinance::ws::WebSocketSession::Config config;
    config.heartbeat_interval_ms = 1000;
    auto session = std::make_shared<cofinance::ws::WebSocketSession>(connection, config);
    std::uint16_t close_code = 0;
    session->setCloseCallback(
        [&close_code](std::uint16_t code, const std::string&) { close_code = code; });
    std::string empty;
    session->start(empty);
    std::string frame = clientFrame(0x2, "binary");
    session->onData(frame);
    CHECK(close_code == 1003);
    loop.pollOnce(100);
}

}  // namespace

int main() {
    std::printf("=== CoFinance WebSocket 单元测试（P9）===\n");
    testHandshake();
    testFrameParser();
    testFrameEncodingAndClose();
    testRealtimeRegistry();
    testSessionFramesAndHeartbeat();
    testSessionRejectsBinary();
    std::printf("=== 检查点 %d 个, 失败 %d 个 ===\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
