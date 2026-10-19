#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cofinance/net/TcpServer.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

bool installHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &action, nullptr) != 0) {
        return false;
    }
    return ::signal(SIGPIPE, SIG_IGN) != SIG_ERR;
}

}  // namespace

int main() {
    if (!installHandlers()) {
        std::fprintf(stderr, "echo server 信号初始化失败\n");
        return EXIT_FAILURE;
    }
    const char* port_env = std::getenv("COFINANCE_SERVER_PORT");
    const int port = port_env == nullptr ? 19006 : std::atoi(port_env);
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "无效端口: %s\n", port_env);
        return EXIT_FAILURE;
    }

    cofinance::net::TcpServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<std::uint16_t>(port);
    cfg.io_threads = 4;
    cfg.max_connections = 5000;

    cofinance::net::TcpServer server(cfg, [](const cofinance::net::TcpConnectionPtr& conn) {
        conn->setMessageCallback([](const cofinance::net::TcpConnectionPtr& connection,
                                    std::string& input) {
            connection->send(input);
            input.clear();
        });
    });
    std::string err;
    if (!server.init(&err)) {
        std::fprintf(stderr, "echo server 初始化失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    if (!server.run([]() { return g_stop != 0; }, &err)) {
        std::fprintf(stderr, "echo server 运行失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
