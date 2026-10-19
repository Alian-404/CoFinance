#pragma once

#include <cstddef>
#include <string>

namespace cofinance {
namespace util {

constexpr int kPbkdf2Iterations = 100000;

// 输出格式：pbkdf2_sha256$100000$<salt-hex>$<hash-hex>
bool hashPassword(const std::string& password, std::string* encoded, std::string* err);

// false + err 为空表示“格式合法但密码不匹配”；err 非空表示哈希损坏或 OpenSSL 失败。
bool verifyPassword(const std::string& password, const std::string& encoded, std::string* err);

// 用户名不存在时也执行同样成本的 PBKDF2，降低通过响应时间枚举账号的风险。
void burnPasswordVerification(const std::string& password);

bool randomHex(std::size_t bytes, std::string* out, std::string* err);
std::string sha256Hex(const std::string& input);

}  // namespace util
}  // namespace cofinance
