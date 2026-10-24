#pragma once

#include <cstdint>
#include <string>

namespace cofinance {

// 禁止拷贝的基类：RAII 资源的默认姿态
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

namespace util {

std::string trim(const std::string& s);
std::string toUpper(const std::string& s);
std::string toLower(const std::string& s);
bool iequals(const std::string& a, const std::string& b);

// 返回 UTF-8 字符数（不是字节数）；非法编码时 *valid=false。
std::size_t utf8Length(const std::string& s, bool* valid = nullptr);

// 时间统一 UTC（ADR-14）：存储、日志、接口全都是 UTC，只在展示层做本地化
int64_t nowUtcMillis();
int64_t nowMonotonicMillis();
std::string formatUtcMillis(int64_t ms, bool with_date = true);
std::string utcDateString(int64_t ms);

// 把 API 的 UTC 时间边界规范化为 MySQL DATETIME 文本。
// 日期形式 YYYY-MM-DD 作为 from 时取 00:00:00，作为 to 时取 23:59:59。
bool parseUtcBoundary(const std::string& raw, bool end_of_day, std::string* db_value,
                      std::string* detail);

// Linux 内核视角的线程 id（不是 std::thread::id）。
// 用途：gdb、perf top、strace 里看到的就是这个数字，排查多线程问题时可对齐
int64_t threadId();

// 只保留文件名的最后一段，日志里不必打全路径
const char* shortFileName(const char* path);

}  // namespace util
}  // namespace cofinance
