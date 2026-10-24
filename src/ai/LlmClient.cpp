#include "cofinance/ai/LlmClient.h"

#include <curl/curl.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "cofinance/util/Logger.h"

namespace cofinance {
namespace ai {
namespace {

using json = nlohmann::json;

std::mutex g_curl_mutex;
int g_curl_refs = 0;
CURLcode g_curl_init_result = CURLE_OK;

bool acquireCurlRuntime() {
    std::lock_guard<std::mutex> lock(g_curl_mutex);
    if (g_curl_refs == 0) {
        g_curl_init_result = ::curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    if (g_curl_init_result != CURLE_OK) {
        return false;
    }
    ++g_curl_refs;
    return true;
}

void releaseCurlRuntime() {
    std::lock_guard<std::mutex> lock(g_curl_mutex);
    if (g_curl_refs == 0) {
        return;
    }
    --g_curl_refs;
    if (g_curl_refs == 0 && g_curl_init_result == CURLE_OK) {
        ::curl_global_cleanup();
    }
}

struct ResponseBuffer {
    std::string body;
    std::size_t limit = 0;
    bool too_large = false;
};

struct ProgressState {
    const std::atomic<bool>* cancelled = nullptr;
};

std::size_t writeResponse(char* data, std::size_t size, std::size_t count, void* userdata) {
    ResponseBuffer* buffer = static_cast<ResponseBuffer*>(userdata);
    const std::size_t bytes = size * count;
    if (buffer == nullptr || bytes > buffer->limit - std::min(buffer->limit, buffer->body.size())) {
        if (buffer != nullptr) {
            buffer->too_large = true;
        }
        return 0;
    }
    buffer->body.append(data, bytes);
    return bytes;
}

int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const ProgressState* state = static_cast<const ProgressState*>(userdata);
    if (state == nullptr || state->cancelled == nullptr) {
        return 0;
    }
    return state->cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}

void setLlmError(LlmError* error, LlmErrorKind kind, std::string message,
                 int http_status = 0) {
    if (error == nullptr) {
        return;
    }
    error->kind = kind;
    error->http_status = http_status;
    error->message = std::move(message);
}

bool validHttpBaseUrl(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

std::string chatUrl(const std::string& base_url) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/chat/completions";
}

}  // namespace

LlmClient::LlmClient(LlmConfig config) : cfg_(std::move(config)) {
    curl_ready_ = acquireCurlRuntime();
}

LlmClient::~LlmClient() {
    if (curl_ready_) {
        releaseCurlRuntime();
    }
}

bool LlmClient::chat(const std::string& system_prompt, const std::string& user_prompt,
                     const std::atomic<bool>& cancelled, LlmReply* reply,
                     LlmError* error) const {
    if (reply == nullptr || error == nullptr) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 输出参数为空");
        return false;
    }
    *reply = LlmReply{};
    *error = LlmError{};
    if (!curl_ready_) {
        setLlmError(error, LlmErrorKind::NotConfigured, "libcurl 初始化失败");
        return false;
    }
    if (cfg_.api_key.empty()) {
        setLlmError(error, LlmErrorKind::NotConfigured, "llm.api_key 未配置");
        return false;
    }
    if (!validHttpBaseUrl(cfg_.base_url)) {
        setLlmError(error, LlmErrorKind::NotConfigured, "llm.base_url 必须以 http:// 或 https:// 开头");
        return false;
    }
    if (cancelled.load(std::memory_order_relaxed)) {
        setLlmError(error, LlmErrorKind::Cancelled, "请求已取消");
        return false;
    }

    const json request{
        {"model", cfg_.model},
        {"messages",
         json::array({{{"role", "system"}, {"content", system_prompt}},
                      {{"role", "user"}, {"content", user_prompt}}})},
        {"temperature", cfg_.temperature},
        {"max_tokens", cfg_.max_tokens},
        {"stream", false},
    };
    const std::string request_body = request.dump();

    CURL* curl = ::curl_easy_init();
    if (curl == nullptr) {
        setLlmError(error, LlmErrorKind::Transport, "创建 libcurl 句柄失败");
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, "Content-Type: application/json");
    const std::string authorization = "Authorization: Bearer " + cfg_.api_key;
    headers = ::curl_slist_append(headers, authorization.c_str());

    ResponseBuffer buffer;
    buffer.limit = cfg_.max_response_bytes;
    ProgressState progress{&cancelled};
    const std::string url = chatUrl(cfg_.base_url);
    char curl_error[CURL_ERROR_SIZE] = {};

    bool options_ok = true;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK;
    options_ok = options_ok &&
                 ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.data()) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                                 static_cast<curl_off_t>(request_body.size())) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                                                 static_cast<long>(cfg_.connect_timeout_ms)) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                                                 static_cast<long>(cfg_.timeout_ms)) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                                                 progressCallback) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error) == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                                                 "http,https") == CURLE_OK;
    options_ok = options_ok && ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK;

    if (!options_ok) {
        ::curl_slist_free_all(headers);
        ::curl_easy_cleanup(curl);
        setLlmError(error, LlmErrorKind::Transport, "设置 libcurl 参数失败");
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    const CURLcode result = ::curl_easy_perform(curl);
    const auto finished = std::chrono::steady_clock::now();
    long http_status = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    reply->http_status = static_cast<int>(http_status);
    reply->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);

    if (cancelled.load(std::memory_order_relaxed) || result == CURLE_ABORTED_BY_CALLBACK) {
        setLlmError(error, LlmErrorKind::Cancelled, "请求已取消", reply->http_status);
        return false;
    }
    if (buffer.too_large) {
        setLlmError(error, LlmErrorKind::ResponseTooLarge, "LLM 响应超过大小上限",
                    reply->http_status);
        return false;
    }
    if (result == CURLE_OPERATION_TIMEDOUT) {
        setLlmError(error, LlmErrorKind::Timeout, "LLM 请求超时", reply->http_status);
        return false;
    }
    if (result != CURLE_OK) {
        const std::string detail = curl_error[0] != '\0' ? curl_error : ::curl_easy_strerror(result);
        setLlmError(error, LlmErrorKind::Transport, detail, reply->http_status);
        return false;
    }
    if (http_status < 200 || http_status >= 300) {
        std::string detail = "上游返回 HTTP " + std::to_string(http_status);
        const json parsed = json::parse(buffer.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            const auto error_it = parsed.find("error");
            if (error_it != parsed.end() && error_it->is_object()) {
                const auto message_it = error_it->find("message");
                if (message_it != error_it->end() && message_it->is_string()) {
                    detail += ": " + message_it->get<std::string>();
                }
            }
        }
        setLlmError(error, LlmErrorKind::HttpStatus, detail, reply->http_status);
        return false;
    }

    const json parsed = json::parse(buffer.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 响应不是 JSON 对象",
                    reply->http_status);
        return false;
    }
    const auto choices = parsed.find("choices");
    if (choices == parsed.end() || !choices->is_array() || choices->empty()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 响应缺少 choices",
                    reply->http_status);
        return false;
    }
    const json& first = (*choices)[0];
    if (!first.is_object()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM choice 格式错误",
                    reply->http_status);
        return false;
    }
    const auto message = first.find("message");
    if (message == first.end() || !message->is_object()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 响应缺少 message",
                    reply->http_status);
        return false;
    }
    const auto content = message->find("content");
    if (content == message->end() || !content->is_string()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 响应缺少文本 answer",
                    reply->http_status);
        return false;
    }
    std::string answer = content->get<std::string>();
    if (answer.empty()) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM 返回空 answer",
                    reply->http_status);
        return false;
    }
    if (answer.size() > cfg_.max_answer_bytes) {
        setLlmError(error, LlmErrorKind::ResponseTooLarge, "LLM answer 超过大小上限",
                    reply->http_status);
        return false;
    }
    bool valid_utf8 = false;
    util::utf8Length(answer, &valid_utf8);
    if (!valid_utf8) {
        setLlmError(error, LlmErrorKind::InvalidResponse, "LLM answer 不是合法 UTF-8",
                    reply->http_status);
        return false;
    }
    reply->answer = std::move(answer);
    return true;
}

}  // namespace ai
}  // namespace cofinance
