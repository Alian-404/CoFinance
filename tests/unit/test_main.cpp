// P0 阶段的极简测试：零第三方依赖的自制断言。
// P3 引入 doctest 后，这里会替换成 doctest 的 TEST_CASE（保持同样的检查点思路）。
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cofinance/util/Common.h"
#include "cofinance/util/Config.h"
#include "cofinance/util/Logger.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (false)

std::string writeTemp(const std::string& name, const std::string& content) {
    const std::string path = "/tmp/cofinance_test_" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return path;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

void testCommonUtil() {
    std::printf("[case] Common 工具函数\n");
    CHECK(cofinance::util::trim("  a b \n") == "a b");
    CHECK(cofinance::util::toUpper("Info") == "INFO");
    CHECK(cofinance::util::iequals("Warn", "WARN"));
    CHECK(cofinance::util::shortFileName("src/util/Logger.cpp") == std::string("Logger.cpp"));
    const int64_t ms = 1758384000000LL;  // 2025-09-20T16:00:00Z
    CHECK(cofinance::util::formatUtcMillis(ms).size() == 24);
    CHECK(cofinance::util::formatUtcMillis(ms).back() == 'Z');
    CHECK(cofinance::util::utcDateString(ms).size() == 10);
    CHECK(cofinance::util::threadId() > 0);
}

void testConfigBasic() {
    std::printf("[case] Config 基本解析\n");
    const std::string path = writeTemp("basic.conf",
                                       "# 整行注释\n"
                                       "server.port = 9007\n"
                                       "\n"
                                       "db.password=P@ss#1\n"
                                       "log.to_terminal=0\n");
    cofinance::Config cfg;
    std::string err;
    CHECK(cfg.loadFile(path, &err));
    CHECK(cfg.getInt("server.port", 0) == 9007);
    // 关键回归点：值里出现的 '#' 不能被当成注释截断，否则密码会被悄悄改错
    CHECK(cfg.get("db.password") == "P@ss#1");
    CHECK(cfg.getBool("log.to_terminal", true) == false);
    CHECK(cfg.get("no.such.key", "fallback") == "fallback");
    CHECK(cfg.has("server.port"));
    CHECK(!cfg.has("no.such.key"));
    CHECK(cofinance::Config::looksSecret("db.password"));
    CHECK(cofinance::Config::looksSecret("llm.api_key"));
    CHECK(cofinance::Config::looksSecret("session.token"));
    CHECK(!cofinance::Config::looksSecret("server.port"));
    CHECK(cofinance::Config::envNameForKey("server.port") == "COFINANCE_SERVER_PORT");
    const std::string dump = cfg.redactedDump();
    CHECK(dump.find("P@ss#1") == std::string::npos);  // 脱敏后不得出现明文密码
    CHECK(dump.find("9007") != std::string::npos);
}

void testConfigErrors() {
    std::printf("[case] Config 错误处理\n");
    std::string err;
    cofinance::Config missing;
    CHECK(!missing.loadFile("/tmp/definitely_not_exist_cofinance.conf", &err));
    CHECK(!err.empty());

    const std::string bad = writeTemp("bad.conf", "这一行没有等号\n");
    cofinance::Config cfg;
    CHECK(!cfg.loadFile(bad, &err));
    CHECK(err.find("bad.conf:1") != std::string::npos);  // 错误信息必须带行号

    const std::string empty_key = writeTemp("empty_key.conf", "=值\n");
    CHECK(!cfg.loadFile(empty_key, &err));
}

void testEnvOverride() {
    std::printf("[case] 环境变量覆盖\n");
    const std::string path = writeTemp("env.conf", "server.port=9006\n");
    cofinance::Config cfg;
    CHECK(cfg.loadFile(path, nullptr));
    CHECK(cfg.getInt("server.port", 0) == 9006);
    ::setenv("COFINANCE_SERVER_PORT", "9100", 1);
    CHECK(cfg.getInt("server.port", 0) == 9100);
    CHECK(cfg.redactedDump().find("来自环境变量") != std::string::npos);
    ::unsetenv("COFINANCE_SERVER_PORT");
    CHECK(cfg.getInt("server.port", 0) == 9006);
}

void testValidate() {
    std::printf("[case] 配置校验（fail fast）\n");
    // 一份「完整且合法」的配置做基线。注意 validate() 的语义是：
    // 范围检查列表里的每一个键都同时是「必填项」，缺一个就报一个错，
    // 这样做的理由是 fail fast —— 少一个键就说明配置文件不完整，不该带着半套配置跑起来。
    const std::string kFull =
        "server.host=0.0.0.0\n"
        "server.port=9006\n"
        "server.backlog=512\n"
        "server.io_threads=4\n"
        "server.max_connections=20000\n"
        "server.keepalive_timeout_ms=60000\n"
        "server.header_limit_bytes=8192\n"
        "server.body_limit_bytes=1048576\n"
        "log.dir=logs\n"
        "log.file_prefix=cofinance\n"
        "log.level=INFO\n"
        "log.queue_size=65536\n"
        "db.host=127.0.0.1\n"
        "db.port=3306\n"
        "db.name=cofinance\n"
        "db.pool_size=32\n"
        "db.connect_timeout_ms=3000\n";

    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    {
        cofinance::Config cfg;
        CHECK(cfg.loadFile(writeTemp("full_ok.conf", kFull), nullptr));
        cfg.validate(&errors, &warnings);
        for (const std::string& e : errors) {
            std::fprintf(stderr, "    意外错误: %s\n", e.c_str());
        }
        CHECK(errors.empty());
        CHECK(!warnings.empty());  // 缺 db.user/db.password → 只告警不报错
    }

    {
        // 只把一个值改错，必须恰好只报这一个错 —— 这能验证「不误报、不串报」
        std::string bad = kFull;
        bad.replace(bad.find("server.port=9006"), std::string("server.port=9006").size(),
                    "server.port=99999");
        cofinance::Config cfg;
        CHECK(cfg.loadFile(writeTemp("bad_port.conf", bad), nullptr));
        errors.clear();
        warnings.clear();
        cfg.validate(&errors, &warnings);
        for (const std::string& e : errors) {
            std::fprintf(stderr, "    报错: %s\n", e.c_str());
        }
        CHECK(errors.size() == 1);
        CHECK(!errors.empty() && errors[0].find("server.port") != std::string::npos);
    }

    {
        // 空配置：范围列表里的键全都算缺失，必须被拒绝
        cofinance::Config empty;
        errors.clear();
        warnings.clear();
        empty.validate(&errors, &warnings);
        CHECK(errors.size() >= 11);
    }

    {
        // 非法日志级别：属于「缺少必填项」之外的语义校验
        std::string bad = kFull;
        bad.replace(bad.find("log.level=INFO"), std::string("log.level=INFO").size(),
                    "log.level=VERBOSE");
        cofinance::Config cfg;
        CHECK(cfg.loadFile(writeTemp("bad_level.conf", bad), nullptr));
        errors.clear();
        warnings.clear();
        cfg.validate(&errors, &warnings);
        CHECK(errors.size() == 1);
    }

    {
        // 连接池过大 → 告警（本机 MySQL max_connections=151）
        std::string big = kFull;
        big.replace(big.find("db.pool_size=32"), std::string("db.pool_size=32").size(),
                    "db.pool_size=200");
        cofinance::Config cfg;
        CHECK(cfg.loadFile(writeTemp("big_pool.conf", big), nullptr));
        errors.clear();
        warnings.clear();
        cfg.validate(&errors, &warnings);
        CHECK(errors.empty());
        bool pool_warned = false;
        for (const std::string& wmsg : warnings) {
            if (wmsg.find("db.pool_size") != std::string::npos) {
                pool_warned = true;
            }
        }
        CHECK(pool_warned);
    }
}

void testLoggerAsync() {
    std::printf("[case] 异步日志\n");
    cofinance::LogConfig lc;
    lc.dir = "/tmp/cofinance_test_logs";
    lc.file_prefix = "unit";
    lc.level = cofinance::LogLevel::DEBUG;
    lc.to_terminal = false;
    lc.queue_size = 4096;

    std::string err;
    CHECK(cofinance::Logger::instance().init(lc, &err));
    CHECK(!err.empty() || err.empty());  // init 成功时 err 应为空
    CHECK(err.empty());
    // 重复 init 必须失败，而不是静默重启一个后台线程
    cofinance::LogConfig lc2 = lc;
    CHECK(!cofinance::Logger::instance().init(lc2, &err));

    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        LOG_INFO("异步日志第 %d 条", i);
    }
    LOG_ERROR("错误级别日志");
    LOG_DEBUG("调试级别日志 %s", "参数拼接");
    // 刻意不断言 writtenCount()==0 这类「中间状态」：后台线程随时可能已经消费掉一部分记录，
    // 瞬时值不确定，断言它 = 制造 flaky test（第一版就踩了这个坑，详见 BUGLOG B-003）。
    // 并发组件的测试只能断言最终状态或偏序关系，不能断言某一瞬间的计数。

    cofinance::Logger::instance().shutdown();
    CHECK(cofinance::Logger::instance().writtenCount() >= kCount + 2);
    CHECK(cofinance::Logger::instance().droppedCount() == 0);

    const std::string logfile =
        lc.dir + "/unit-" + cofinance::util::utcDateString(cofinance::util::nowUtcMillis()) + ".log";
    const std::string content = readAll(logfile);
    CHECK(!content.empty());
    CHECK(content.find("异步日志第 0 条") != std::string::npos);
    CHECK(content.find("异步日志第 199 条") != std::string::npos);  // shutdown 必须排空队列
    CHECK(content.find("[ERROR]") != std::string::npos);
    CHECK(content.find("[DEBUG]") != std::string::npos);
    CHECK(content.find("Z [INFO] [tid=") != std::string::npos);  // UTC 时间戳 + 线程 id
}

}  // namespace

int main() {
    std::printf("=== CoFinance 单元测试（P0）===\n");
    testCommonUtil();
    testConfigBasic();
    testConfigErrors();
    testEnvOverride();
    testValidate();
    testLoggerAsync();
    std::printf("=== 检查点 %d 个, 失败 %d 个 ===\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
