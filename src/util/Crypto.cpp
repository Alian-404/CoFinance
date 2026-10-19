#include "cofinance/util/Crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace cofinance {
namespace util {
namespace {

constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kDigestBytes = 32;

std::string toHex(const unsigned char* data, std::size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool fromHex(const std::string& text, std::vector<unsigned char>* out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    out->clear();
    out->reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out->push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

bool derive(const std::string& password, const std::vector<unsigned char>& salt, int iterations,
            std::vector<unsigned char>* digest) {
    if (password.size() > static_cast<std::size_t>(INT_MAX) ||
        salt.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    digest->assign(kDigestBytes, 0);
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                             static_cast<int>(salt.size()), iterations, EVP_sha256(),
                             static_cast<int>(digest->size()), digest->data()) == 1;
}

}  // namespace

bool hashPassword(const std::string& password, std::string* encoded, std::string* err) {
    if (encoded == nullptr) {
        if (err != nullptr) {
            *err = "密码哈希输出参数为空";
        }
        return false;
    }
    unsigned char salt[kSaltBytes];
    if (RAND_bytes(salt, static_cast<int>(sizeof(salt))) != 1) {
        if (err != nullptr) {
            *err = "生成密码盐失败";
        }
        return false;
    }
    const std::vector<unsigned char> salt_vec(salt, salt + sizeof(salt));
    std::vector<unsigned char> digest;
    if (!derive(password, salt_vec, kPbkdf2Iterations, &digest)) {
        if (err != nullptr) {
            *err = "PBKDF2 计算失败";
        }
        return false;
    }
    *encoded = "pbkdf2_sha256$" + std::to_string(kPbkdf2Iterations) + "$" +
               toHex(salt, sizeof(salt)) + "$" + toHex(digest.data(), digest.size());
    if (err != nullptr) {
        err->clear();
    }
    return true;
}

bool verifyPassword(const std::string& password, const std::string& encoded, std::string* err) {
    std::size_t first = encoded.find('$');
    std::size_t second = first == std::string::npos ? first : encoded.find('$', first + 1);
    std::size_t third = second == std::string::npos ? second : encoded.find('$', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
        encoded.compare(0, first, "pbkdf2_sha256") != 0) {
        if (err != nullptr) {
            *err = "密码哈希格式非法";
        }
        return false;
    }

    const std::string iter_text = encoded.substr(first + 1, second - first - 1);
    if (iter_text.empty() || iter_text.size() > 7) {
        if (err != nullptr) {
            *err = "密码哈希迭代次数非法";
        }
        return false;
    }
    int iterations = 0;
    for (char c : iter_text) {
        if (c < '0' || c > '9') {
            if (err != nullptr) {
                *err = "密码哈希迭代次数非法";
            }
            return false;
        }
        iterations = iterations * 10 + (c - '0');
    }
    if (iterations < 10000 || iterations > 1000000) {
        if (err != nullptr) {
            *err = "密码哈希迭代次数超出安全范围";
        }
        return false;
    }

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!fromHex(encoded.substr(second + 1, third - second - 1), &salt) ||
        salt.size() < 8 || !fromHex(encoded.substr(third + 1), &expected) ||
        expected.size() < 16) {
        if (err != nullptr) {
            *err = "密码哈希内容非法";
        }
        return false;
    }

    std::vector<unsigned char> actual;
    if (!derive(password, salt, iterations, &actual) || actual.size() != expected.size()) {
        if (err != nullptr) {
            *err = "PBKDF2 校验失败";
        }
        return false;
    }
    if (err != nullptr) {
        err->clear();
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

void burnPasswordVerification(const std::string& password) {
    static const unsigned char salt[16] = {0x43, 0x6f, 0x46, 0x69, 0x6e, 0x61, 0x6e,
                                           0x63, 0x65, 0x2d, 0x64, 0x75, 0x6d, 0x6d,
                                           0x79, 0x21};
    std::vector<unsigned char> digest;
    derive(password, std::vector<unsigned char>(salt, salt + sizeof(salt)), kPbkdf2Iterations,
           &digest);
}

bool randomHex(std::size_t bytes, std::string* out, std::string* err) {
    if (out == nullptr || bytes == 0 || bytes > static_cast<std::size_t>(INT_MAX)) {
        if (err != nullptr) {
            *err = "随机字节数非法";
        }
        return false;
    }
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        if (err != nullptr) {
            *err = "安全随机数生成失败";
        }
        return false;
    }
    *out = toHex(buffer.data(), buffer.size());
    if (err != nullptr) {
        err->clear();
    }
    return true;
}

std::string sha256Hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
        return std::string();
    }
    return toHex(digest, length);
}

}  // namespace util
}  // namespace cofinance
