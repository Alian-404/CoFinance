#include "cofinance/util/Logger.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

namespace cofinance {
namespace {

const char* colorFor(LogLevel lv) {
    switch (lv) {
        case LogLevel::DEBUG: return "\033[90m";   // 灰
        case LogLevel::INFO:  return "\033[32m";   // 绿
        case LogLevel::WARN:  return "\033[33m";   // 黄
        case LogLevel::ERROR: return "\033[31m";   // 红
        case LogLevel::FATAL: return "\033[1;31m"; // 亮红加粗
        case LogLevel::OFF:   return "";
    }
    return "";
}

bool stdoutIsTty() {
    static const bool tty = (::isatty(STDOUT_FILENO) == 1);
    return tty;
}

}  // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    shutdown();
}

const char* Logger::levelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        case LogLevel::OFF:   return "OFF";
    }
    return "?";
}

LogLevel Logger::levelFromString(const std::string& s, bool* ok) {
    const std::string up = util::toUpper(util::trim(s));
    static const struct { const char* name; LogLevel lv; } kMap[] = {
        {"DEBUG", LogLevel::DEBUG}, {"INFO", LogLevel::INFO}, {"WARN", LogLevel::WARN},
        {"WARNING", LogLevel::WARN}, {"ERROR", LogLevel::ERROR}, {"FATAL", LogLevel::FATAL},
        {"OFF", LogLevel::OFF},
    };
    for (const auto& m : kMap) {
        if (up == m.name) {
            if (ok != nullptr) {
                *ok = true;
            }
            return m.lv;
        }
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return LogLevel::INFO;
}

void Logger::setLevel(LogLevel lv) {
    level_.store(static_cast<int>(lv), std::memory_order_relaxed);
}

LogLevel Logger::level() const {
    return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
}

bool Logger::enabled(LogLevel lv) const {
    if (lv == LogLevel::OFF) {
        return false;
    }
    return static_cast<int>(lv) >= level_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::droppedCount() const {
    return dropped_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::writtenCount() const {
    return written_.load(std::memory_order_relaxed);
}

std::string Logger::filePathFor(const LogConfig& cfg, int64_t now_ms) {
    return cfg.dir + "/" + cfg.file_prefix + "-" + util::utcDateString(now_ms) + ".log";
}

bool Logger::init(const LogConfig& cfg, std::string* err) {
    if (running_.load(std::memory_order_acquire)) {
        if (err != nullptr) {
            *err = "Logger 已经初始化过了";
        }
        return false;
    }
    cfg_ = cfg;
    cfg_.queue_size = cfg_.queue_size == 0 ? 65536 : cfg_.queue_size;
    level_.store(static_cast<int>(cfg_.level), std::memory_order_relaxed);

    std::error_code ec;
    std::filesystem::create_directories(cfg_.dir, ec);
    if (ec) {
        if (err != nullptr) {
            *err = "创建日志目录失败: " + cfg_.dir + " (" + ec.message() + ")";
        }
        return false;
    }

    // 在主线程先把文件打开：配置写错要在 init 阶段就暴露，
    // 而不是等后台线程第一次写日志时才发现，那时没人接住这个错误
    const int64_t now = util::nowUtcMillis();
    const std::string path = filePathFor(cfg_, now);
    fp_ = std::fopen(path.c_str(), "a");
    if (fp_ == nullptr) {
        if (err != nullptr) {
            *err = "打开日志文件失败: " + path + " (" + std::strerror(errno) + ")";
        }
        return false;
    }
    std::setvbuf(fp_, nullptr, _IOFBF, 256 * 1024);  // 256KB 全缓冲，减少 write 系统调用
    cur_date_ = util::utcDateString(now);

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Logger::workerLoop, this);
    return true;
}

void Logger::shutdown() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
        cv_consumer_.notify_all();
        cv_producer_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    flushFile();
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void Logger::flushFile() {
    if (fp_ != nullptr) {
        std::fflush(fp_);
    }
}

void Logger::fallbackWrite(const std::string& line) {
    // 未初始化或已关闭：只写 stderr，绝不触碰 fp_（避免与后台线程竞争）
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void Logger::log(LogLevel lv, const char* file, int line, const char* fmt, ...) {
    if (!enabled(lv)) {
        return;
    }

    // 两趟处理：先问需要多长，再按需分配。消息被静默截断是排查现场最讨厌的事之一
    va_list ap;
    va_start(ap, fmt);
    const int need = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);

    std::vector<char> msg(need > 0 ? static_cast<std::size_t>(need) + 1 : 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(msg.data(), msg.size(), fmt, ap);
    va_end(ap);

    std::string out;
    out.reserve(msg.size() + 96);
    out += util::formatUtcMillis(util::nowUtcMillis());   // UTC（ADR-14）
    out += " [";
    out += levelName(lv);
    out += "] [tid=";
    out += std::to_string(util::threadId());
    out += "] [";
    out += util::shortFileName(file);
    out += ':';
    out += std::to_string(line);
    out += "] ";
    out.append(msg.data());
    out.push_back('\n');

    enqueue(lv, std::move(out));
}

void Logger::enqueue(LogLevel lv, std::string&& line) {
    if (!running_.load(std::memory_order_acquire)) {
        fallbackWrite(line);
        return;
    }

    std::unique_lock<std::mutex> lk(mtx_);
    if (queue_.size() >= cfg_.queue_size) {
        if (static_cast<int>(lv) >= static_cast<int>(LogLevel::ERROR)) {
            cv_producer_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                return queue_.size() < cfg_.queue_size || !running_.load(std::memory_order_relaxed);
            });
        }
        if (queue_.size() >= cfg_.queue_size) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            if (static_cast<int>(lv) >= static_cast<int>(LogLevel::ERROR)) {
                fallbackWrite(line);  // 致命信息绝不舍弃
            }
            return;
        }
    }
    queue_.emplace_back(lv, std::move(line));
    cv_consumer_.notify_one();
}

void Logger::workerLoop() {
    for (;;) {
        LogLevel lv = LogLevel::DEBUG;
        std::string line;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (!cv_consumer_.wait_for(lk, std::chrono::milliseconds(1000), [this] {
                    return !queue_.empty() || !running_.load(std::memory_order_relaxed);
                })) {
                // 空闲 1 秒：主动刷盘，保证 `tail -f` 能看到实时日志
                flushFile();
                continue;
            }
            if (queue_.empty()) {
                if (!running_.load(std::memory_order_relaxed)) {
                    break;  // 队列已排空且被要求停止 —— 优雅退出，不丢日志
                }
                continue;
            }
            lv = queue_.front().first;
            line = std::move(queue_.front().second);
            queue_.pop_front();
            cv_producer_.notify_all();
        }
        writeToSink(lv, line);
        if (static_cast<int>(lv) >= static_cast<int>(LogLevel::ERROR)) {
            flushFile();  // 错误必须马上可见，万一进程随后崩溃也不丢
        }
    }
    flushFile();
}

bool Logger::rollFileIfNeeded(int64_t now_ms) {
    const std::string today = util::utcDateString(now_ms);
    if (today == cur_date_) {
        return false;
    }
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    const std::string path = filePathFor(cfg_, now_ms);
    fp_ = std::fopen(path.c_str(), "a");
    if (fp_ == nullptr) {
        std::fprintf(stderr, "[logger] 跨天滚动日志文件失败: %s (%s)\n", path.c_str(),
                     std::strerror(errno));
    } else {
        std::setvbuf(fp_, nullptr, _IOFBF, 256 * 1024);
        cur_date_ = today;
    }
    return true;
}

void Logger::writeToSink(LogLevel lv, const std::string& line) {
    rollFileIfNeeded(util::nowUtcMillis());
    if (fp_ != nullptr) {
        std::fwrite(line.data(), 1, line.size(), fp_);
    }
    if (cfg_.to_terminal) {
        if (stdoutIsTty()) {
            std::fwrite(colorFor(lv), 1, std::strlen(colorFor(lv)), stdout);
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fwrite("\033[0m", 1, 4, stdout);
        } else {
            std::fwrite(line.data(), 1, line.size(), stdout);
        }
    }
    written_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace cofinance
