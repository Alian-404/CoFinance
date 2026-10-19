#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "cofinance/util/Common.h"

namespace cofinance {

enum class LogLevel : int { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4, OFF = 5 };

struct LogConfig {
    std::string dir = "logs";
    std::string file_prefix = "cofinance";
    LogLevel level = LogLevel::INFO;
    bool to_terminal = true;
    std::size_t queue_size = 65536;
};

// 异步日志。
//
// 设计要点（详见 docs/learning/PN-00）：
//   * 业务线程只做「格式化 + 入队」，落盘由唯一的后台线程完成；
//   * 队列有界：无界队列在故障时会变成内存炸弹，把「日志打太多」升级成 OOM；
//   * 背压策略分级：DEBUG/WARN 满了直接丢弃并计数；ERROR/FATAL 最多等 100ms，
//     仍满则退回同步写 stderr —— 宁可拖慢，不可丢失致命信息；
//   * 唯一的全局单例。日志是典型的横切关注点，逐个传递 Logger& 只会污染所有签名。
class Logger : private NonCopyable {
public:
    static Logger& instance();

    bool init(const LogConfig& cfg, std::string* err);
    void shutdown();

    void setLevel(LogLevel lv);
    LogLevel level() const;
    bool enabled(LogLevel lv) const;

    void log(LogLevel lv, const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

    std::uint64_t droppedCount() const;
    std::uint64_t writtenCount() const;

    static LogLevel levelFromString(const std::string& s, bool* ok = nullptr);
    static const char* levelName(LogLevel lv);

private:
    Logger() = default;
    ~Logger();

    void enqueue(LogLevel lv, std::string&& line);
    void workerLoop();
    void writeToSink(LogLevel lv, const std::string& line);
    bool rollFileIfNeeded(int64_t now_ms);
    void flushFile();
    void fallbackWrite(const std::string& line);
    static std::string filePathFor(const LogConfig& cfg, int64_t now_ms);

    LogConfig cfg_;
    std::atomic<int> level_{static_cast<int>(LogLevel::INFO)};
    std::atomic<bool> running_{false};

    std::mutex mtx_;
    std::condition_variable cv_producer_;
    std::condition_variable cv_consumer_;
    std::deque<std::pair<LogLevel, std::string>> queue_;

    std::thread worker_;
    FILE* fp_ = nullptr;      // 只允许后台线程访问（init/shutdown 中除外，此时无并发）
    std::string cur_date_;    // 同上
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> written_{0};
};

#define COF_LOG_IMPL(lv, ...)                                                          \
    do {                                                                               \
        ::cofinance::Logger& cof_logger_ = ::cofinance::Logger::instance();            \
        if (cof_logger_.enabled(lv)) {                                                 \
            cof_logger_.log(lv, __FILE__, __LINE__, __VA_ARGS__);                      \
        }                                                                              \
    } while (false)

#define LOG_DEBUG(...) COF_LOG_IMPL(::cofinance::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...) COF_LOG_IMPL(::cofinance::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) COF_LOG_IMPL(::cofinance::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) COF_LOG_IMPL(::cofinance::LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) COF_LOG_IMPL(::cofinance::LogLevel::FATAL, __VA_ARGS__)

}  // namespace cofinance
