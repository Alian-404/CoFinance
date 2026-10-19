#include "cofinance/util/Common.h"

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <ctime>
#include <cstring>
#include <string>

namespace cofinance {
namespace {

// 手工拼接定宽数字，代替 snprintf("%04d-...").
//
// 起因：最初用 char buf[16] + snprintf("%04d-%02d-%02d") 输出日期，
// 被 -Werror=format-truncation 拦下 —— 编译器按 int 的全值域推导最坏情况
// （tm_year 若是 2147483647，光年份就要 11 个字符），16 字节缓冲区并不安全。
// 改成手工拼接后：既没有截断风险，也保证输出宽度恒定（日志时间戳对齐、
// 日志文件名可按字典序排序都依赖这一点）。
void appendPadded(std::string& out, long long value, unsigned width) {
    if (value < 0) {
        value = 0;
    }
    char tmp[24];
    unsigned len = 0;
    do {
        tmp[len++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value > 0 && len < sizeof(tmp));
    while (len < width && len < sizeof(tmp)) {
        tmp[len++] = '0';
    }
    while (len > 0) {
        out.push_back(tmp[--len]);
    }
}

int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

namespace util {

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string toUpper(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return r;
}

std::string toLower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

int64_t nowUtcMillis() {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t nowMonotonicMillis() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string formatUtcMillis(int64_t ms, bool with_date) {
    const std::time_t sec = static_cast<std::time_t>(ms / 1000);
    const int milli = static_cast<int>(ms % 1000);
    struct tm tmv {};
    if (gmtime_r(&sec, &tmv) == nullptr) {
        return with_date ? std::string("1970-01-01T00:00:00.000Z")
                         : std::string("00:00:00.000");
    }

    std::string out;
    out.reserve(24);
    if (with_date) {
        appendPadded(out, clampInt(tmv.tm_year + 1900, 1000, 9999), 4);
        out.push_back('-');
        appendPadded(out, clampInt(tmv.tm_mon + 1, 1, 12), 2);
        out.push_back('-');
        appendPadded(out, clampInt(tmv.tm_mday, 1, 31), 2);
        out.push_back('T');
    }
    appendPadded(out, clampInt(tmv.tm_hour, 0, 23), 2);
    out.push_back(':');
    appendPadded(out, clampInt(tmv.tm_min, 0, 59), 2);
    out.push_back(':');
    appendPadded(out, clampInt(tmv.tm_sec, 0, 60), 2);  // 60 是闰秒，合法
    out.push_back('.');
    appendPadded(out, milli < 0 ? 0 : milli, 3);
    if (with_date) {
        out.push_back('Z');
    }
    return out;
}

std::string utcDateString(int64_t ms) {
    const std::time_t sec = static_cast<std::time_t>(ms / 1000);
    struct tm tmv {};
    if (gmtime_r(&sec, &tmv) == nullptr) {
        return std::string("1970-01-01");
    }
    std::string out;
    out.reserve(10);
    appendPadded(out, clampInt(tmv.tm_year + 1900, 1000, 9999), 4);
    out.push_back('-');
    appendPadded(out, clampInt(tmv.tm_mon + 1, 1, 12), 2);
    out.push_back('-');
    appendPadded(out, clampInt(tmv.tm_mday, 1, 31), 2);
    return out;
}

int64_t threadId() {
    // 每个线程只做一次系统调用，之后走 thread_local 缓存
    static thread_local const int64_t tid = static_cast<int64_t>(::syscall(SYS_gettid));
    return tid;
}

const char* shortFileName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

}  // namespace util
}  // namespace cofinance
