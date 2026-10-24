#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "cofinance/net/EventLoop.h"
#include "cofinance/net/Socket.h"
#include "cofinance/net/TcpConnection.h"
#include "cofinance/util/Common.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                            \
    } while (false)

void testSocketRoundTrip() {
    std::printf("[case] 监听、accept 与 fd RAII\n");
    std::string err;
    cofinance::net::Socket listener =
        cofinance::net::Socket::listenTcp("127.0.0.1", 0, 16, &err);
    CHECK(listener.valid());
    CHECK(err.empty());

    const std::uint16_t port = listener.localPort();
    CHECK(port != 0);

    cofinance::net::Socket client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    CHECK(client.valid());

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    CHECK(::connect(client.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    sockaddr_storage peer {};
    socklen_t peer_len = sizeof(peer);
    const int accepted_fd =
        cofinance::net::acceptNonBlocking(listener.fd(), &peer, &peer_len, &err);
    CHECK(accepted_fd >= 0);
    cofinance::net::Socket accepted(accepted_fd);

    const char payload[] = "ping";
    CHECK(::send(client.fd(), payload, 4, MSG_NOSIGNAL) == 4);
    char buffer[8] {};
    CHECK(::recv(accepted.fd(), buffer, 4, 0) == 4);
    CHECK(std::memcmp(buffer, payload, 4) == 0);
    CHECK(cofinance::net::formatPeer(peer).find("127.0.0.1:") == 0);
}

void testEventLoopTaskAndTimer() {
    std::printf("[case] eventfd 跨线程任务与 timerfd\n");
    cofinance::net::EventLoop loop;
    std::string err;
    CHECK(loop.init(&err));
    CHECK(err.empty());

    bool task_ran = false;
    loop.runInLoop([&task_ran]() { task_ran = true; });
    CHECK(loop.pollOnce(200) == cofinance::net::Epoller::WaitResult::Ready);
    CHECK(task_ran);

    bool timer_fired = false;
    loop.addTimer(20, [&timer_fired]() { timer_fired = true; });
    const std::int64_t deadline = cofinance::util::nowMonotonicMillis() + 1000;
    while (!timer_fired && cofinance::util::nowMonotonicMillis() < deadline) {
        loop.pollOnce(100);
    }
    CHECK(timer_fired);

    bool canceled_fired = false;
    const cofinance::net::TimerQueue::TimerId id =
        loop.addTimer(20, [&canceled_fired]() { canceled_fired = true; });
    CHECK(loop.cancelTimer(id));
    loop.pollOnce(60);
    CHECK(!canceled_fired);
}

void testConnectionIdleTimeout() {
    std::printf("[case] 连接空闲超时\n");
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    cofinance::net::Socket peer(fds[0]);

    cofinance::net::EventLoop loop;
    std::string err;
    CHECK(loop.init(&err));

    bool closed = false;
    bool appended = false;
    auto connection = std::make_shared<cofinance::net::TcpConnection>(&loop, fds[1], "socketpair",
                                                                     30);
    connection->setCloseCallback([&closed](int) { closed = true; });
    connection->addCloseCallback([&appended](int) { appended = true; });
    CHECK(connection->start(&err));

    const std::int64_t deadline = cofinance::util::nowMonotonicMillis() + 1000;
    while (!closed && cofinance::util::nowMonotonicMillis() < deadline) {
        loop.pollOnce(100);
    }
    CHECK(closed);
    CHECK(appended);
}

}  // namespace

int main() {
    std::printf("=== CoFinance 网络层单元测试（P1）===\n");
    testSocketRoundTrip();
    testEventLoopTaskAndTimer();
    testConnectionIdleTimeout();
    std::printf("=== 检查点 %d 个, 失败 %d 个 ===\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
