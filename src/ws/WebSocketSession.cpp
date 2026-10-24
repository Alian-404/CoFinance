#include "cofinance/ws/WebSocketSession.h"

#include <exception>
#include <string>
#include <utility>

#include "cofinance/net/TcpConnection.h"
#include "cofinance/util/Common.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace ws {

WebSocketSession::WebSocketSession(std::weak_ptr<net::TcpConnection> connection, Config config)
    : connection_(std::move(connection)), config_(config),
      parser_(config.max_message_bytes == 0 ? 1 : config.max_message_bytes) {
    if (config_.max_message_bytes == 0) {
        config_.max_message_bytes = 1;
    }
    if (config_.heartbeat_interval_ms < 1) {
        config_.heartbeat_interval_ms = 1;
    }
    if (config_.idle_timeout_ms < config_.heartbeat_interval_ms) {
        config_.idle_timeout_ms = config_.heartbeat_interval_ms;
    }
}

WebSocketSession::~WebSocketSession() {
    finalize(1006, "connection closed");
}

void WebSocketSession::start(std::string& initial_input) {
    if (started_ || closed_) {
        initial_input.clear();
        return;
    }
    started_ = true;
    last_activity_ms_ = util::nowMonotonicMillis();
    scheduleHeartbeat();
    if (open_callback_) {
        try {
            open_callback_();
        } catch (const std::exception& e) {
            LOG_ERROR("WebSocket open 回调异常：%s", e.what());
            close(1011, "open handler failed");
            initial_input.clear();
            return;
        } catch (...) {
            LOG_ERROR("WebSocket open 回调抛出未知异常");
            close(1011, "open handler failed");
            initial_input.clear();
            return;
        }
    }
    if (!initial_input.empty()) {
        onData(initial_input);
    }
}

void WebSocketSession::onData(std::string& input) {
    if (!started_ || closed_) {
        input.clear();
        return;
    }
    incoming_.append(input);
    input.clear();
    last_activity_ms_ = util::nowMonotonicMillis();

    while (!closed_ && !incoming_.empty()) {
        Frame frame;
        std::uint16_t close_code = 1002;
        std::string error;
        const ParseResult result = parser_.parse(incoming_, &frame, &close_code, &error);
        if (result == ParseResult::NeedMore) {
            return;
        }
        if (result == ParseResult::Error) {
            incoming_.clear();
            close(close_code, error);
            return;
        }
        handleFrame(frame);
    }
}

void WebSocketSession::onClose() {
    incoming_.clear();
    fragment_payload_.clear();
    finalize(1006, "connection closed");
}

void WebSocketSession::postText(std::string text) {
    auto connection = connection_.lock();
    if (!connection || connection->closed() || closed_) {
        return;
    }
    std::weak_ptr<WebSocketSession> weak = weak_from_this();
    connection->runInLoop([weak, text = std::move(text)]() mutable {
        if (std::shared_ptr<WebSocketSession> self = weak.lock()) {
            self->sendText(text);
        }
    });
}

bool WebSocketSession::sendText(std::string_view text) {
    return sendFrame(Opcode::Text, text);
}

bool WebSocketSession::sendPing(std::string_view payload) {
    if (payload.size() > 125) {
        payload = payload.substr(0, 125);
    }
    return sendFrame(Opcode::Ping, payload);
}

bool WebSocketSession::sendFrame(Opcode opcode, std::string_view payload) {
    if (closed_ || !started_) {
        return false;
    }
    auto connection = connection_.lock();
    if (!connection || connection->closed()) {
        finalize(1006, "connection closed");
        return false;
    }
    connection->send(encodeFrame(opcode, payload, true));
    return !connection->closed();
}

void WebSocketSession::handleFrame(const Frame& frame) {
    if (frame.opcode == Opcode::Close) {
        std::uint16_t code = 1000;
        std::string reason;
        if (!parseClosePayload(frame.payload, &code, &reason)) {
            close(1002, "invalid close frame");
            return;
        }
        close(code, reason);
        return;
    }
    if (frame.opcode == Opcode::Ping) {
        sendFrame(Opcode::Pong, frame.payload);
        return;
    }
    if (frame.opcode == Opcode::Pong) {
        return;
    }
    handleDataFrame(frame);
}

void WebSocketSession::handleDataFrame(const Frame& frame) {
    if (frame.opcode == Opcode::Binary) {
        close(1003, "binary messages are not supported");
        return;
    }
    if (frame.opcode == Opcode::Text) {
        if (fragment_active_) {
            close(1002, "new data frame while fragmented message is open");
            return;
        }
        if (frame.fin) {
            deliverText(frame.payload);
            return;
        }
        fragment_active_ = true;
        fragment_opcode_ = Opcode::Text;
        fragment_payload_ = frame.payload;
        return;
    }
    if (frame.opcode != Opcode::Continuation) {
        close(1002, "unexpected data opcode");
        return;
    }
    if (!fragment_active_ || fragment_opcode_ != Opcode::Text) {
        close(1002, "continuation without fragmented message");
        return;
    }
    if (fragment_payload_.size() > config_.max_message_bytes - frame.payload.size()) {
        close(1009, "fragmented message too large");
        return;
    }
    fragment_payload_ += frame.payload;
    if (!frame.fin) {
        return;
    }
    std::string complete = std::move(fragment_payload_);
    fragment_payload_.clear();
    fragment_active_ = false;
    fragment_opcode_ = Opcode::Continuation;
    deliverText(std::move(complete));
}

void WebSocketSession::deliverText(std::string message) {
    if (message.size() > config_.max_message_bytes) {
        close(1009, "message too large");
        return;
    }
    bool valid_utf8 = false;
    util::utf8Length(message, &valid_utf8);
    if (!valid_utf8) {
        close(1007, "invalid UTF-8");
        return;
    }
    if (!text_callback_) {
        return;
    }
    try {
        text_callback_(message);
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket text 回调异常：%s", e.what());
        close(1011, "message handler failed");
    } catch (...) {
        LOG_ERROR("WebSocket text 回调抛出未知异常");
        close(1011, "message handler failed");
    }
}

void WebSocketSession::scheduleHeartbeat() {
    if (closed_ || config_.heartbeat_interval_ms <= 0) {
        return;
    }
    auto connection = connection_.lock();
    if (!connection || connection->closed()) {
        finalize(1006, "connection closed");
        return;
    }
    std::weak_ptr<WebSocketSession> weak = weak_from_this();
    heartbeat_timer_id_ = connection->addTimer(
        config_.heartbeat_interval_ms, [weak]() {
            if (std::shared_ptr<WebSocketSession> self = weak.lock()) {
                self->heartbeat_timer_id_ = 0;
                self->heartbeat();
            }
        });
}

void WebSocketSession::heartbeat() {
    if (closed_) {
        return;
    }
    const std::int64_t now = util::nowMonotonicMillis();
    if (config_.idle_timeout_ms > 0 &&
        now - last_activity_ms_ >= config_.idle_timeout_ms) {
        close(1001, "heartbeat timeout");
        return;
    }
    if (!sendPing(std::to_string(now))) {
        return;
    }
    scheduleHeartbeat();
}

void WebSocketSession::close(std::uint16_t code, std::string reason) {
    if (closed_) {
        return;
    }
    auto connection = connection_.lock();
    if (connection && !connection->closed()) {
        connection->send(encodeCloseFrame(code, reason));
        connection->shutdownAfterWrite();
    }
    finalize(code, reason);
}

void WebSocketSession::finalize(std::uint16_t code, const std::string& reason) {
    if (close_notified_) {
        return;
    }
    close_notified_ = true;
    closed_ = true;
    if (heartbeat_timer_id_ != 0) {
        if (auto connection = connection_.lock()) {
            connection->cancelTimer(heartbeat_timer_id_);
        }
        heartbeat_timer_id_ = 0;
    }
    CloseCallback callback = std::move(close_callback_);
    close_callback_ = nullptr;
    text_callback_ = nullptr;
    open_callback_ = nullptr;
    if (callback) {
        try {
            callback(code, reason);
        } catch (const std::exception& e) {
            LOG_ERROR("WebSocket close 回调异常：%s", e.what());
        } catch (...) {
            LOG_ERROR("WebSocket close 回调抛出未知异常");
        }
    }
}

}  // namespace ws
}  // namespace cofinance
