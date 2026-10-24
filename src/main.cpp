#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cofinance/api/GroupApi.h"
#include "cofinance/api/LedgerApi.h"
#include "cofinance/api/StatsApi.h"
#include "cofinance/api/UserApi.h"
#include "cofinance/db/ConnectionPool.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/net/TcpServer.h"
#include "cofinance/service/GroupService.h"
#include "cofinance/service/LedgerService.h"
#include "cofinance/service/SettlementService.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"
#include "cofinance/util/Config.h"
#include "cofinance/util/Logger.h"

#ifndef COFINANCE_VERSION
#define COFINANCE_VERSION "0.0.0-dev"
#endif
#ifndef COFINANCE_BUILD_TYPE
#define COFINANCE_BUILD_TYPE "unknown"
#endif

namespace {

// 信号处理器里能安全调用的函数非常有限（async-signal-safe），
// printf / malloc / 日志 都不是。所以这里只写一个 sig_atomic_t，
// 真正的日志与清理逻辑全部回到主循环里执行。
volatile std::sig_atomic_t g_stop = 0;

void onSignal(int /*sig*/) {
    g_stop = 1;
}

void printUsage(const char* prog) {
    std::printf(
        "CoFinance %s —— 共同财务管理系统（C++17 后端）\n\n"
        "用法: %s [选项]\n"
        "  -c, --config <path>   配置文件路径（默认 config/server.conf）\n"
        "  -h, --help            显示本帮助\n"
        "  -v, --version         显示版本号\n",
        COFINANCE_VERSION, prog);
}

bool installSignalHandlers(std::string* err) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // 不设 SA_RESTART：让阻塞中的系统调用返回 EINTR，主循环能立刻响应信号

    if (::sigaction(SIGINT, &sa, nullptr) != 0 || ::sigaction(SIGTERM, &sa, nullptr) != 0) {
        *err = std::string("注册 SIGINT/SIGTERM 处理器失败: ") + std::strerror(errno);
        return false;
    }
    // 网络服务必须忽略 SIGPIPE：向已关闭的连接写数据时，默认动作是把整个进程杀掉。
    // 正确做法是忽略它，然后靠 write() 返回的 EPIPE/errno 去处理这条连接（P1 会用到）。
    if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        *err = std::string("忽略 SIGPIPE 失败: ") + std::strerror(errno);
        return false;
    }
    return true;
}

std::string dirOf(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/server.conf";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (arg == "-v" || arg == "--version") {
            std::printf("CoFinance %s (build=%s)\n", COFINANCE_VERSION, COFINANCE_BUILD_TYPE);
            return EXIT_SUCCESS;
        }
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: %s 需要一个参数\n", arg.c_str());
                return 2;
            }
            config_path = argv[++i];
            continue;
        }
        std::fprintf(stderr, "错误: 未知参数 '%s'（用 -h 查看用法）\n", arg.c_str());
        return 2;
    }

    // ---------- 1. 配置 ----------
    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err)) {
        std::fprintf(stderr, "启动失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    const std::string secrets_path = dirOf(config_path) + "/secrets.conf";
    if (std::filesystem::exists(secrets_path)) {
        if (!cfg.loadFile(secrets_path, &err)) {
            std::fprintf(stderr, "启动失败: %s\n", err.c_str());
            return EXIT_FAILURE;
        }
    } else {
        std::fprintf(stderr, "[WARN] 未找到 %s —— P3 连接数据库前必须补上（可从 secrets.conf.example 复制）\n",
                     secrets_path.c_str());
    }

    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    cfg.validate(&errors, &warnings);
    if (!errors.empty()) {
        std::fprintf(stderr, "配置校验失败，拒绝启动（fail fast，不带着错配置跑起来）：\n");
        for (const std::string& e : errors) {
            std::fprintf(stderr, "  - %s\n", e.c_str());
        }
        return EXIT_FAILURE;
    }
    for (const std::string& w : warnings) {
        std::fprintf(stderr, "[WARN] %s\n", w.c_str());
    }

    // ---------- 2. 日志 ----------
    cofinance::LogConfig log_cfg;
    log_cfg.dir = cfg.get("log.dir", "logs");
    log_cfg.file_prefix = cfg.get("log.file_prefix", "cofinance");
    bool level_ok = false;
    log_cfg.level = cofinance::Logger::levelFromString(cfg.get("log.level", "INFO"), &level_ok);
    log_cfg.to_terminal = cfg.getBool("log.to_terminal", true);
    log_cfg.queue_size = static_cast<std::size_t>(cfg.getInt("log.queue_size", 65536));

    std::string logger_err;
    if (!cofinance::Logger::instance().init(log_cfg, &logger_err)) {
        std::fprintf(stderr, "启动失败: %s\n", logger_err.c_str());
        return EXIT_FAILURE;
    }

    // ---------- 3. 信号 ----------
    if (!installSignalHandlers(&err)) {
        LOG_FATAL("信号处理初始化失败: %s", err.c_str());
        cofinance::Logger::instance().shutdown();
        return EXIT_FAILURE;
    }

    // ---------- 4. 启动横幅 ----------
    const int64_t start_ms = cofinance::util::nowUtcMillis();
    LOG_INFO("CoFinance %s 启动（pid=%d, build=%s）", COFINANCE_VERSION,
             static_cast<int>(::getpid()), COFINANCE_BUILD_TYPE);
    LOG_INFO("配置文件: %s", config_path.c_str());
    LOG_INFO("工作目录: %s", std::filesystem::current_path().string().c_str());
    LOG_INFO("异步日志: 级别=%s, 目录=%s, 队列=%zu, 终端输出=%s",
             cofinance::Logger::levelName(log_cfg.level), log_cfg.dir.c_str(),
             log_cfg.queue_size, log_cfg.to_terminal ? "on" : "off");
    LOG_INFO("生效配置（敏感项已打码）:\n%s", cfg.redactedDump().c_str());
    LOG_INFO("P7 结算系统启动：统计聚合 + 净额 + 最少笔数建议 + 结算落库");
    LOG_DEBUG("这条 DEBUG 日志用于验证日志级别过滤，生产环境把 log.level 调成 INFO 就不会输出");

    // ---------- 5. MySQL 连接池 ----------
    cofinance::db::MySqlConfig db_cfg;
    db_cfg.host = cfg.get("db.host", "127.0.0.1");
    db_cfg.port = static_cast<unsigned int>(cfg.getInt("db.port", 3306));
    db_cfg.user = cfg.get("db.user");
    db_cfg.password = cfg.get("db.password");
    db_cfg.database = cfg.get("db.name", "cofinance");
    db_cfg.pool_size = static_cast<std::size_t>(cfg.getInt("db.pool_size", 32));
    db_cfg.connect_timeout_ms = static_cast<int>(cfg.getInt("db.connect_timeout_ms", 3000));
    db_cfg.acquire_timeout_ms = static_cast<int>(cfg.getInt("db.acquire_timeout_ms", 3000));

    cofinance::db::ConnectionPool db_pool;
    if (!db_pool.init(db_cfg, &err)) {
        LOG_FATAL("数据库初始化失败: %s", err.c_str());
        cofinance::Logger::instance().shutdown();
        return EXIT_FAILURE;
    }
    LOG_INFO("MySQL 连接池就绪: %s:%u/%s, 连接数=%zu", db_cfg.host.c_str(), db_cfg.port,
             db_cfg.database.c_str(), db_pool.capacity());

    cofinance::service::UserService user_service(db_pool);
    cofinance::service::GroupService group_service(db_pool);
    cofinance::service::LedgerService ledger_service(db_pool);
    cofinance::service::SettlementService settlement_service(db_pool);

    // ---------- 6. HTTP 与主从 Reactor ----------
    cofinance::http::HttpServerConfig http_cfg;
    http_cfg.header_limit =
        static_cast<std::size_t>(cfg.getInt("server.header_limit_bytes", 8192));
    http_cfg.body_limit =
        static_cast<std::size_t>(cfg.getInt("server.body_limit_bytes", 1048576));
    http_cfg.request_timeout_ms =
        static_cast<int>(cfg.getInt("server.request_timeout_ms", 10000));
    http_cfg.static_root = cfg.get("server.static_root", "www");

    cofinance::http::HttpServer http_server(http_cfg);
    if (!http_server.init(&err)) {
        LOG_FATAL("HTTP 层初始化失败: %s", err.c_str());
        cofinance::Logger::instance().shutdown();
        return EXIT_FAILURE;
    }
    http_server.setHealthCheck([&db_pool](std::string* check_err) {
        return db_pool.healthCheck(check_err);
    });
    cofinance::api::UserApi::registerRoutes(http_server, user_service);
    cofinance::api::GroupApi::registerRoutes(http_server, user_service, group_service);
    cofinance::api::LedgerApi::registerRoutes(http_server, user_service, ledger_service);
    cofinance::api::StatsApi::registerRoutes(http_server, user_service, settlement_service);

    cofinance::net::TcpServerConfig net_cfg;
    net_cfg.host = cfg.get("server.host", "0.0.0.0");
    net_cfg.port = static_cast<std::uint16_t>(cfg.getInt("server.port", 9006));
    net_cfg.backlog = static_cast<int>(cfg.getInt("server.backlog", 512));
    net_cfg.io_threads = static_cast<int>(cfg.getInt("server.io_threads", 4));
    net_cfg.max_connections = static_cast<std::size_t>(cfg.getInt("server.max_connections", 20000));
    net_cfg.idle_timeout_ms =
        static_cast<int>(cfg.getInt("server.keepalive_timeout_ms", 60000));

    cofinance::net::TcpServer server(net_cfg, [&http_server](const cofinance::net::TcpConnectionPtr& conn) {
        http_server.onConnection(conn);
    });
    if (!server.init(&err)) {
        LOG_FATAL("网络层初始化失败: %s", err.c_str());
        cofinance::Logger::instance().shutdown();
        return EXIT_FAILURE;
    }
    LOG_INFO("监听地址: %s:%u", net_cfg.host.c_str(), static_cast<unsigned>(server.port()));

    if (!server.run([]() { return g_stop != 0; }, &err)) {
        LOG_FATAL("服务运行失败: %s", err.c_str());
        const int64_t uptime_ms = cofinance::util::nowUtcMillis() - start_ms;
        LOG_INFO("异常退出统计: 运行 %lld ms", static_cast<long long>(uptime_ms));
        cofinance::Logger::instance().shutdown();
        return EXIT_FAILURE;
    }

    // ---------- 7. 优雅退出 ----------
    db_pool.shutdown();
    LOG_INFO("收到退出信号，开始优雅退出（排空日志队列 → 刷盘 → 关闭文件）");
    const int64_t uptime_ms = cofinance::util::nowUtcMillis() - start_ms;
    LOG_INFO("退出统计: 运行 %lld ms, 累计写日志 %llu 条, 丢弃 %llu 条",
             static_cast<long long>(uptime_ms),
             static_cast<unsigned long long>(cofinance::Logger::instance().writtenCount()),
             static_cast<unsigned long long>(cofinance::Logger::instance().droppedCount()));
    cofinance::Logger::instance().shutdown();

    std::printf("CoFinance 已优雅退出（运行 %lld ms）\n", static_cast<long long>(uptime_ms));
    return EXIT_SUCCESS;
}
