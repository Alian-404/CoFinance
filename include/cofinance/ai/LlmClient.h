#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace ai {

struct LlmConfig {
    std::string base_url = "https://api.deepseek.com";
    std::string api_key;
    std::string model = "deepseek-chat";
    int connect_timeout_ms = 3000;
    int timeout_ms = 10000;
    int max_tokens = 600;
    double temperature = 0.3;
    std::size_t max_response_bytes = 256 * 1024;
    std::size_t max_answer_bytes = 16 * 1024;
};

enum class LlmErrorKind {
    None,
    NotConfigured,
    Timeout,
    Cancelled,
    Transport,
    HttpStatus,
    InvalidResponse,
    ResponseTooLarge,
};

struct LlmError {
    LlmErrorKind kind = LlmErrorKind::None;
    int http_status = 0;
    std::string message;
};

struct LlmReply {
    std::string answer;
    int http_status = 0;
    std::int64_t elapsed_ms = 0;
};

// DeepSeek 的 OpenAI 兼容 Chat Completions 客户端。
// 每个实例可以被多个工作线程并发调用；curl easy handle 在每次请求内独立创建。
class LlmClient : private NonCopyable {
public:
    explicit LlmClient(LlmConfig config);
    ~LlmClient();

    bool configured() const noexcept { return curl_ready_ && !cfg_.api_key.empty(); }

    bool chat(const std::string& system_prompt, const std::string& user_prompt,
              const std::atomic<bool>& cancelled, LlmReply* reply, LlmError* error) const;

    const LlmConfig& config() const noexcept { return cfg_; }

private:
    LlmConfig cfg_;
    bool curl_ready_ = false;
};

}  // namespace ai
}  // namespace cofinance
