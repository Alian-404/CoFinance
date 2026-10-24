#include "cofinance/service/AiService.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/service/SettlementService.h"
#include "cofinance/util/Crypto.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {
namespace {

constexpr std::int64_t kRateWindowMs = 60000;
constexpr std::int64_t kDayMs = 86400000;

struct MonthRange {
    std::string current_from;
    std::string current_to;
    std::string previous_from;
    std::string previous_to;
};

std::string dateString(int year, int month, int day) {
    char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return buffer;
}

int daysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month != 2) {
        return kDays[month - 1];
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
}

bool utcDateParts(std::int64_t now_ms, int* year, int* month, int* day) {
    const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
    std::tm value {};
    if (::gmtime_r(&seconds, &value) == nullptr) {
        return false;
    }
    *year = value.tm_year + 1900;
    *month = value.tm_mon + 1;
    *day = value.tm_mday;
    return true;
}

bool buildMonthRange(std::int64_t now_ms, MonthRange* range) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (range == nullptr || !utcDateParts(now_ms, &year, &month, &day)) {
        return false;
    }
    int previous_year = year;
    int previous_month = month - 1;
    if (previous_month == 0) {
        previous_month = 12;
        --previous_year;
    }
    range->current_from = dateString(year, month, 1);
    range->current_to = dateString(year, month, day);
    range->previous_from = dateString(previous_year, previous_month, 1);
    range->previous_to = dateString(previous_year, previous_month,
                                    daysInMonth(previous_year, previous_month));
    return true;
}

void setError(ServiceError* error, int status, int code, std::string message) {
    if (error == nullptr) {
        return;
    }
    error->http_status = status;
    error->code = code;
    error->message = std::move(message);
}

bool validQuestion(const std::string& question, int max_chars, ServiceError* error) {
    const std::string cleaned = util::trim(question);
    if (cleaned.empty()) {
        setError(error, 400, 1001, "问题不能为空");
        return false;
    }
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(cleaned, &valid_utf8);
    if (!valid_utf8) {
        setError(error, 400, 1001, "问题必须是合法 UTF-8");
        return false;
    }
    if (length > static_cast<std::size_t>(max_chars)) {
        setError(error, 400, 1001, "问题不能超过 " + std::to_string(max_chars) + " 个字符");
        return false;
    }
    for (char c : cleaned) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc < 0x20 && c != '\n' && c != '\t') || uc == 0x7f) {
            setError(error, 400, 1001, "问题不能包含控制字符");
            return false;
        }
    }
    return true;
}

}  // namespace

AiService::AiService(SettlementService& settlements, AiConfig config)
    : settlements_(settlements),
      config_(std::move(config)),
      llm_client_(std::make_unique<ai::LlmClient>(config_.llm)) {}

AiService::AiService(SettlementService& settlements, AiConfig config, LlmHandler handler)
    : settlements_(settlements),
      config_(std::move(config)),
      llm_handler_(std::move(handler)) {}

AiService::~AiService() {
    shutdown();
}

bool AiService::init(std::string* err) {
    if (running_) {
        return true;
    }
    if (config_.worker_threads <= 0 || config_.queue_capacity == 0 ||
        config_.rate_limit_per_minute <= 0 || config_.max_question_chars <= 0) {
        if (err != nullptr) {
            *err = "AI 线程池、队列、限流或问题长度配置非法";
        }
        return false;
    }
    if (!llm_handler_ && llm_client_ && !llm_client_->configured()) {
        LOG_WARN("AI 助手未配置 llm.api_key；接口会返回 503，但服务其余功能不受影响");
    }

    running_ = true;
    try {
        workers_.reserve(static_cast<std::size_t>(config_.worker_threads));
        for (int i = 0; i < config_.worker_threads; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    } catch (const std::exception& e) {
        shutdown();
        if (err != nullptr) {
            *err = std::string("创建 AI 线程失败: ") + e.what();
        }
        return false;
    }
    return true;
}

void AiService::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!running_ && workers_.empty()) {
            return;
        }
        running_ = false;
        queue_.clear();
    }
    queue_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::shared_ptr<AiCancellation> AiService::askAsync(std::uint64_t user_id,
                                                    std::uint64_t group_id,
                                                    std::string question,
                                                    AiCompletion completion,
                                                    ServiceError* error) {
    if (!validQuestion(question, config_.max_question_chars, error)) {
        return nullptr;
    }
    return submit(Mode::Ask, user_id, group_id, util::trim(question),
                  std::move(completion), error);
}

std::shared_ptr<AiCancellation> AiService::suggestionsAsync(std::uint64_t user_id,
                                                            std::uint64_t group_id,
                                                            AiCompletion completion,
                                                            ServiceError* error) {
    return submit(Mode::Suggestions, user_id, group_id, std::string(), std::move(completion),
                  error);
}

std::shared_ptr<AiCancellation> AiService::submit(Mode mode, std::uint64_t user_id,
                                                  std::uint64_t group_id, std::string question,
                                                  AiCompletion completion,
                                                  ServiceError* error) {
    if (!completion) {
        setError(error, 500, 2001, "服务器内部错误");
        return nullptr;
    }
    if (!running_) {
        setError(error, 503, 2002, "AI 服务暂不可用，请稍后重试");
        return nullptr;
    }
    if (!rateLimitAllows(user_id)) {
        setError(error, 429, 3001, "AI 请求过于频繁，请稍后再试");
        return nullptr;
    }

    auto job = std::make_shared<Job>();
    job->mode = mode;
    job->user_id = user_id;
    job->group_id = group_id;
    job->question = std::move(question);
    job->completion = std::move(completion);
    job->cancellation = std::make_shared<AiCancellation>();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!running_) {
            setError(error, 503, 2002, "AI 服务正在停止");
            return nullptr;
        }
        if (queue_.size() >= config_.queue_capacity) {
            setError(error, 503, 2002, "AI 服务繁忙，请稍后重试");
            return nullptr;
        }
        queue_.push_back(job);
    }
    queue_cv_.notify_one();
    return job->cancellation;
}

void AiService::workerLoop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
            if (!running_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!job || jobCancelled(*job)) {
            continue;
        }

        AiResult result;
        try {
            execute(*job, &result);
        } catch (const std::exception& e) {
            LOG_ERROR("AI 任务异常: %s", e.what());
            result.ok = false;
            setError(&result.error, 500, 2001, "AI 服务内部错误");
        } catch (...) {
            LOG_ERROR("AI 任务异常: unknown");
            result.ok = false;
            setError(&result.error, 500, 2001, "AI 服务内部错误");
        }

        if (jobCancelled(*job) || !running_) {
            continue;
        }
        try {
            job->completion(std::move(result));
        } catch (const std::exception& e) {
            LOG_ERROR("AI 完成回调异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("AI 完成回调异常: unknown");
        }
    }
}

void AiService::execute(const Job& job, AiResult* result) {
    const auto started = std::chrono::steady_clock::now();
    ai::AiPromptContext context;
    if (!loadContext(job.user_id, job.group_id, &context, &result->error)) {
        result->ok = false;
        return;
    }

    const bool suggestions = job.mode == Mode::Suggestions;
    const std::string user_prompt = suggestions
                                        ? ai::PromptBuilder::suggestionsPrompt(context)
                                        : ai::PromptBuilder::askPrompt(context, job.question);
    const std::string cache_key = std::to_string(job.group_id) + ":" +
                                  util::sha256Hex(user_prompt);
    if (suggestions) {
        AiAnswer cached;
        if (loadCachedSuggestion(cache_key, &cached)) {
            cached.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
            result->ok = true;
            result->answer = std::move(cached);
            return;
        }
    }

    ai::LlmReply reply;
    ai::LlmError llm_error;
    const bool llm_ok = llm_handler_
                            ? llm_handler_(ai::PromptBuilder::systemPrompt(), user_prompt,
                                           job.cancellation->flag(), &reply, &llm_error)
                            : llm_client_->chat(ai::PromptBuilder::systemPrompt(), user_prompt,
                                                job.cancellation->flag(), &reply, &llm_error);
    if (!llm_ok) {
        if (llm_error.kind == ai::LlmErrorKind::Cancelled || jobCancelled(job)) {
            return;
        }
        LOG_WARN("LLM 调用失败 kind=%d http=%d elapsed=%lldms: %s",
                 static_cast<int>(llm_error.kind), llm_error.http_status,
                 static_cast<long long>(reply.elapsed_ms), llm_error.message.c_str());
        result->ok = false;
        result->error = mapLlmError(llm_error);
        return;
    }
    if (jobCancelled(job)) {
        return;
    }

    AiAnswer answer;
    answer.answer = std::move(reply.answer);
    answer.cached = false;
    answer.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    answer.current_from = context.current_from;
    answer.current_to = context.current_to;
    answer.model = config_.llm.model;
    if (!llm_handler_ && llm_client_) {
        answer.model = llm_client_->config().model;
    }
    if (suggestions) {
        storeCachedSuggestion(cache_key, answer);
    }
    result->ok = true;
    result->answer = std::move(answer);
}

bool AiService::loadContext(std::uint64_t user_id, std::uint64_t group_id,
                            ai::AiPromptContext* context, ServiceError* error) {
    MonthRange range;
    if (!buildMonthRange(util::nowUtcMillis(), &range)) {
        setError(error, 500, 2001, "无法计算统计时间范围");
        return false;
    }

    StatsInput current_input;
    current_input.from = range.current_from;
    current_input.to = range.current_to;
    if (!settlements_.getStats(user_id, group_id, current_input, &context->current, error)) {
        return false;
    }
    StatsInput previous_input;
    previous_input.from = range.previous_from;
    previous_input.to = range.previous_to;
    if (!settlements_.getStats(user_id, group_id, previous_input, &context->previous, error)) {
        return false;
    }
    context->current_from = range.current_from;
    context->current_to = range.current_to;
    context->previous_from = range.previous_from;
    context->previous_to = range.previous_to;
    return true;
}

bool AiService::rateLimitAllows(std::uint64_t user_id) {
    const std::int64_t now = util::nowMonotonicMillis();
    std::lock_guard<std::mutex> lock(rate_mutex_);
    std::deque<std::int64_t>& window = rate_windows_[user_id];
    while (!window.empty() && now - window.front() >= kRateWindowMs) {
        window.pop_front();
    }
    if (window.size() >= static_cast<std::size_t>(config_.rate_limit_per_minute)) {
        return false;
    }
    window.push_back(now);

    if (rate_windows_.size() > 4096) {
        for (auto it = rate_windows_.begin(); it != rate_windows_.end();) {
            if (it->first != user_id && (it->second.empty() ||
                                         now - it->second.back() >= kRateWindowMs)) {
                it = rate_windows_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

bool AiService::loadCachedSuggestion(const std::string& key, AiAnswer* answer) {
    const std::int64_t now = util::nowUtcMillis();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto found = suggestion_cache_.find(key);
    if (found == suggestion_cache_.end()) {
        return false;
    }
    if (found->second.expires_at_ms <= now) {
        suggestion_cache_.erase(found);
        return false;
    }
    *answer = found->second.answer;
    answer->cached = true;
    return true;
}

void AiService::storeCachedSuggestion(const std::string& key, const AiAnswer& answer) {
    const std::int64_t now = util::nowUtcMillis();
    CacheEntry entry;
    entry.answer = answer;
    entry.answer.cached = false;
    entry.expires_at_ms = (now / kDayMs + 1) * kDayMs;
    entry.inserted_at_ms = now;

    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto it = suggestion_cache_.begin(); it != suggestion_cache_.end();) {
        if (it->second.expires_at_ms <= now) {
            it = suggestion_cache_.erase(it);
        } else {
            ++it;
        }
    }
    if (suggestion_cache_.size() >= config_.suggestion_cache_capacity) {
        auto oldest = suggestion_cache_.begin();
        for (auto it = suggestion_cache_.begin(); it != suggestion_cache_.end(); ++it) {
            if (it->second.inserted_at_ms < oldest->second.inserted_at_ms) {
                oldest = it;
            }
        }
        if (oldest != suggestion_cache_.end()) {
            suggestion_cache_.erase(oldest);
        }
    }
    suggestion_cache_[key] = std::move(entry);
}

ServiceError AiService::mapLlmError(const ai::LlmError& error) {
    ServiceError out;
    switch (error.kind) {
        case ai::LlmErrorKind::NotConfigured:
            out.http_status = 503;
            out.code = 2002;
            out.message = "AI 助手未配置或暂不可用";
            break;
        case ai::LlmErrorKind::Timeout:
            out.http_status = 504;
            out.code = 2002;
            out.message = "AI 服务响应超时，请稍后重试";
            break;
        default:
            out.http_status = 503;
            out.code = 2002;
            out.message = "AI 服务暂时不可用，请稍后重试";
            break;
    }
    return out;
}

bool AiService::jobCancelled(const Job& job) {
    return job.cancellation != nullptr && job.cancellation->cancelled();
}

std::size_t AiService::pendingJobs() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

std::size_t AiService::cacheSize() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return suggestion_cache_.size();
}

}  // namespace service
}  // namespace cofinance
