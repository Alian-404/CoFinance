#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace cofinance {

// 配置解析器。
//
// 取值优先级（高 → 低）：
//   1. 环境变量 COFINANCE_<KEY>     例如 server.port -> COFINANCE_SERVER_PORT
//   2. secrets.conf（后加载的覆盖先加载的）
//   3. server.conf
//   4. 代码里的默认值
//
// 文件语法：key=value，整行以 # 开头是注释；空行忽略。
// 特意不支持「行内注释」——否则 db.password=P@ss#1 会被悄悄截断成 P@ss，
// 这类问题排查起来极痛苦，宁可少一个便利特性。
class Config {
public:
    bool loadFile(const std::string& path, std::string* err);

    bool has(const std::string& key) const;
    std::string get(const std::string& key, const std::string& def = std::string()) const;
    long long getInt(const std::string& key, long long def) const;
    bool getBool(const std::string& key, bool def) const;

    // 启动即校验：错误 → 拒绝启动（fail fast）；警告 → 打印但继续
    void validate(std::vector<std::string>* errors, std::vector<std::string>* warnings) const;

    // 打印全部配置用于启动日志；含 password/secret/key/token 的键自动打码
    std::string redactedDump() const;

    static bool looksSecret(const std::string& key);
    static std::string envNameForKey(const std::string& key);

private:
    std::unordered_map<std::string, std::string> kv_;
};

}  // namespace cofinance
