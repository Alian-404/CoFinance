#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "cofinance/util/Common.h"
#include "cofinance/ws/WebSocketProtocol.h"

namespace cofinance {
namespace net {
class TcpConnection;
}
namespace ws {

// 一个已完成 HTTP Upgrade 的 WebSocket 连接。所有会话方法都在连接所属
// EventLoop 线程执行；postText() 可以从任意线程调用，它只负责投递任务。
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>,
                         private NonCopyable {
public:
    struct Config {
        std::size_t max_message_bytes = 64 * 1024;
        int heartbeat_interval_ms = 30000;
        int idle_timeout_ms = 90000;
    };

    using OpenCallback = std::function<void()>;
    using TextCallback = std::function<void(const std::string&)>;
    using CloseCallback = std::function<void(std::uint16_t, const std::string&)>;

    WebSocketSession(std::weak_ptr<net::TcpConnection> connection, Config config);
    ~WebSocketSession();

    void setOpenCallback(OpenCallback callback) { open_callback_ = std::move(callback); }
    void setTextCallback(TextCallback callback) { text_callback_ = std::move(callback); }
    void setCloseCallback(CloseCallback callback) { close_callback_ = std::move(callback); }

    // initial_input 是 HTTP 头之后、同一次 read 中已经到达的帧字节。
    void start(std::string& initial_input);
    void onData(std::string& input);
    void onClose();

    void postText(std::string text);
    bool sendText(std::string_view text);
    bool sendPing(std::string_view payload = {});
    void close(std::uint16_t code, std::string reason);

    bool closed() const noexcept { return closed_; }

private:
    void handleFrame(const Frame& frame);
    void handleDataFrame(const Frame& frame);
    void deliverText(std::string message);
    bool sendFrame(Opcode opcode, std::string_view payload);
    void scheduleHeartbeat();
    void heartbeat();
    void finalize(std::uint16_t code, const std::string& reason);

    std::weak_ptr<net::TcpConnection> connection_;
    Config config_;
    FrameParser parser_;
    std::string incoming_;
    std::string fragment_payload_;
    Opcode fragment_opcode_ = Opcode::Continuation;
    bool fragment_active_ = false;
    bool started_ = false;
    bool closed_ = false;
    bool close_notified_ = false;
    std::uint64_t heartbeat_timer_id_ = 0;
    std::int64_t last_activity_ms_ = 0;
    OpenCallback open_callback_;
    TextCallback text_callback_;
    CloseCallback close_callback_;
};

}  // namespace ws
}  // namespace cofinance
