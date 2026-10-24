#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cofinance/ai/LlmClient.h"
#include "cofinance/ai/PromptBuilder.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace service {

class SettlementService;

struct AiConfig {
    int worker_threads = 2;
    std::size_t queue_capacity = 64;
    int rate_limit_per_minute = 5;
    int max_question_chars = 500;
    std::size_t suggestion_cache_capacity = 512;
    ai::LlmConfig llm;
};

class AiCancellation : private NonCopyable {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    const std::atomic<bool>& flag() const noexcept { return cancelled_; }

private:
    std::atomic<bool> cancelled_{false};
};

struct AiAnswer {
    std::string answer;
    bool cached = false;
    std::int64_t elapsed_ms = 0;
    std::string current_from;
    std::string current_to;
    std::string model;
};

struct AiResult {
    bool ok = false;
    AiAnswer answer;
    ServiceError error;
};

using AiCompletion = std::function<void(AiResult)>;
using LlmHandler = std::function<bool(const std::string&, const std::string&,
                                      const std::atomic<bool>&, ai::LlmReply*, ai::LlmError*)>;

// AI 请求的独立有界线程池。HTTP IO 线程只负责鉴权、参数校验与入队；
// 统计查询、prompt 构造和外部 LLM 网络等待都不占用 Reactor 线程。
class AiService : private NonCopyable {
public:
    AiService(SettlementService& settlements, AiConfig config);
    AiService(SettlementService& settlements, AiConfig config, LlmHandler handler);
    ~AiService();

    bool init(std::string* err);
    void shutdown();

    std::shared_ptr<AiCancellation> askAsync(std::uint64_t user_id, std::uint64_t group_id,
                                             std::string question, AiCompletion completion,
                                             ServiceError* error);
    std::shared_ptr<AiCancellation> suggestionsAsync(std::uint64_t user_id,
                                                     std::uint64_t group_id,
                                                     AiCompletion completion,
                                                     ServiceError* error);

    std::size_t pendingJobs() const;
    std::size_t cacheSize() const;

private:
    enum class Mode { Ask, Suggestions };

    struct Job {
        Mode mode = Mode::Ask;
        std::uint64_t user_id = 0;
        std::uint64_t group_id = 0;
        std::string question;
        AiCompletion completion;
        std::shared_ptr<AiCancellation> cancellation;
    };

    struct CacheEntry {
        AiAnswer answer;
        std::int64_t expires_at_ms = 0;
        std::int64_t inserted_at_ms = 0;
    };

    std::shared_ptr<AiCancellation> submit(Mode mode, std::uint64_t user_id,
                                           std::uint64_t group_id, std::string question,
                                           AiCompletion completion, ServiceError* error);
    void workerLoop();
    void execute(const Job& job, AiResult* result);
    bool loadContext(std::uint64_t user_id, std::uint64_t group_id,
                     ai::AiPromptContext* context, ServiceError* error);
    bool rateLimitAllows(std::uint64_t user_id);
    bool loadCachedSuggestion(const std::string& key, AiAnswer* answer);
    void storeCachedSuggestion(const std::string& key, const AiAnswer& answer);
    static bool jobCancelled(const Job& job);
    static ServiceError mapLlmError(const ai::LlmError& error);

    SettlementService& settlements_;
    AiConfig config_;
    std::unique_ptr<ai::LlmClient> llm_client_;
    LlmHandler llm_handler_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::thread> workers_;
    bool running_ = false;

    mutable std::mutex rate_mutex_;
    std::unordered_map<std::uint64_t, std::deque<std::int64_t>> rate_windows_;

    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, CacheEntry> suggestion_cache_;
};

}  // namespace service
}  // namespace cofinance
