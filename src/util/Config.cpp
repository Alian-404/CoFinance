#include "cofinance/util/Config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace {

bool parseBoolValue(const std::string& raw, bool* out) {
    const std::string v = util::toLower(util::trim(raw));
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        *out = false;
        return true;
    }
    return false;
}

}  // namespace

std::string Config::envNameForKey(const std::string& key) {
    std::string s = "COFINANCE_";
    s.reserve(key.size() + 10);
    for (const char c : key) {
        s.push_back((c == '.' || c == '-') ? '_'
                                           : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return s;
}

bool Config::loadFile(const std::string& path, std::string* err) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (err != nullptr) {
            *err = "无法打开配置文件: " + path;
        }
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = util::trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            if (err != nullptr) {
                *err = path + ":" + std::to_string(lineno) + " 缺少 '=' : " + trimmed;
            }
            return false;
        }
        const std::string key = util::trim(trimmed.substr(0, eq));
        const std::string value = util::trim(trimmed.substr(eq + 1));
        if (key.empty()) {
            if (err != nullptr) {
                *err = path + ":" + std::to_string(lineno) + " 键名为空";
            }
            return false;
        }
        kv_[key] = value;
    }
    return true;
}

bool Config::has(const std::string& key) const {
    if (std::getenv(envNameForKey(key).c_str()) != nullptr) {
        return true;
    }
    return kv_.find(key) != kv_.end();
}

std::string Config::get(const std::string& key, const std::string& def) const {
    if (const char* env = std::getenv(envNameForKey(key).c_str())) {
        return std::string(env);
    }
    const auto it = kv_.find(key);
    return it != kv_.end() ? it->second : def;
}

long long Config::getInt(const std::string& key, long long def) const {
    const std::string raw = get(key);
    if (raw.empty()) {
        return def;
    }
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(raw.c_str(), &end, 10);
    if (errno != 0 || end == raw.c_str() || *end != '\0') {
        return def;
    }
    return v;
}

bool Config::getBool(const std::string& key, bool def) const {
    bool out = def;
    if (!parseBoolValue(get(key), &out)) {
        return def;
    }
    return out;
}

void Config::validate(std::vector<std::string>* errors,
                      std::vector<std::string>* warnings) const {
    auto err = [&](const std::string& msg) {
        if (errors != nullptr) {
            errors->push_back(msg);
        }
    };
    auto warn = [&](const std::string& msg) {
        if (warnings != nullptr) {
            warnings->push_back(msg);
        }
    };

    struct RangeCheck {
        const char* key;
        long long def;
        long long lo;
        long long hi;
    };
    static const RangeCheck kRanges[] = {
        {"server.port", 9006, 1, 65535},
        {"server.backlog", 512, 1, 65535},
        {"server.io_threads", 4, 1, 256},
        {"server.max_connections", 20000, 1, 1000000},
        {"server.keepalive_timeout_ms", 60000, 1000, 3600000},
        {"server.request_timeout_ms", 10000, 100, 600000},
        {"server.header_limit_bytes", 8192, 512, 1048576},
        {"server.body_limit_bytes", 1048576, 1024, 104857600},
        {"log.queue_size", 65536, 1024, 10485760},
        {"db.port", 3306, 1, 65535},
        {"db.pool_size", 32, 1, 1000},
        {"db.connect_timeout_ms", 3000, 100, 60000},
    };
    for (const RangeCheck& rc : kRanges) {
        const std::string raw = get(rc.key);
        if (raw.empty()) {
            err(std::string("缺少必填配置项: ") + rc.key);
            continue;
        }
        const long long v = getInt(rc.key, rc.def);
        if (v < rc.lo || v > rc.hi) {
            err(std::string(rc.key) + "=" + raw + " 超出合法范围 [" + std::to_string(rc.lo) +
                ", " + std::to_string(rc.hi) + "]");
        }
    }

    static const char* const kRequiredString[] = {
        "server.host", "log.dir", "log.file_prefix", "log.level", "db.host", "db.name",
    };
    for (const char* key : kRequiredString) {
        if (get(key).empty()) {
            err(std::string("缺少必填配置项: ") + key);
        }
    }

    static const char* const kLevels[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    bool level_ok = false;
    const std::string lv = util::toUpper(get("log.level"));
    for (const char* cand : kLevels) {
        if (lv == cand) {
            level_ok = true;
            break;
        }
    }
    if (!lv.empty() && !level_ok) {
        err("log.level=" + get("log.level") + " 不是合法级别（DEBUG/INFO/WARN/ERROR/FATAL/OFF）");
    }

    // P0 阶段还没有数据库模块，凭据缺失只告警；P3 接线后会升级为错误
    if (get("db.user").empty() || get("db.password").empty()) {
        warn("db.user / db.password 未配置（P0 阶段不影响启动，P3 连接数据库时必需）");
    }
    if (getInt("db.pool_size", 32) > 100) {
        warn("db.pool_size 大于 100：本机 MySQL max_connections=151，过大的连接池会把数据库自己压垮");
    }
}

bool Config::looksSecret(const std::string& key) {
    const std::string k = util::toLower(key);
    static const char* const kMarks[] = {"password", "passwd", "secret", "key", "token"};
    for (const char* m : kMarks) {
        if (k.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string Config::redactedDump() const {
    std::vector<std::string> keys;
    keys.reserve(kv_.size());
    for (const auto& kv : kv_) {
        keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());

    std::ostringstream oss;
    for (const std::string& k : keys) {
        const bool from_env = std::getenv(envNameForKey(k).c_str()) != nullptr;
        oss << "  " << k << " = ";
        if (looksSecret(k)) {
            oss << "***（已打码）";
        } else {
            oss << get(k);
        }
        if (from_env) {
            oss << "   [来自环境变量]";
        }
        oss << "\n";
    }
    return oss.str();
}

}  // namespace cofinance
