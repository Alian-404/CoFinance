#include "cofinance/db/ConnectionPool.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/PreparedStatement.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace db {

struct ConnectionPool::Impl {
    MySqlConfig config;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::vector<std::unique_ptr<MySqlConnection>> idle;
    std::size_t target_size = 0;
    bool closing = false;
};

void ConnectionPool::recycle(const std::shared_ptr<Impl>& state,
                             std::unique_ptr<MySqlConnection> connection) {
    if (connection == nullptr || state == nullptr) {
        return;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->closing) {
        lock.unlock();
        connection.reset();
        return;
    }
    state->idle.push_back(std::move(connection));
    lock.unlock();
    state->available.notify_one();
}

ConnectionPool::Lease::Lease(std::shared_ptr<Impl> state,
                             std::unique_ptr<MySqlConnection> connection)
    : state_(std::move(state)), connection_(std::move(connection)) {}

ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), connection_(std::move(other.connection_)) {}

ConnectionPool::Lease& ConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        connection_ = std::move(other.connection_);
    }
    return *this;
}

ConnectionPool::Lease::~Lease() {
    release();
}

MySqlConnection& ConnectionPool::Lease::connection() {
    return *connection_;
}

const MySqlConnection& ConnectionPool::Lease::connection() const {
    return *connection_;
}

ConnectionPool::Lease::operator bool() const {
    return connection_ != nullptr && connection_->connected();
}

void ConnectionPool::Lease::release() {
    if (connection_ == nullptr) {
        state_.reset();
        return;
    }
    std::shared_ptr<Impl> state = std::move(state_);
    std::unique_ptr<MySqlConnection> connection = std::move(connection_);
    recycle(state, std::move(connection));
}

ConnectionPool::ConnectionPool() = default;

ConnectionPool::~ConnectionPool() {
    shutdown();
}

bool ConnectionPool::init(const MySqlConfig& config, std::string* err) {
    shutdown();
    if (config.pool_size == 0) {
        if (err != nullptr) {
            *err = "db.pool_size 必须大于 0";
        }
        return false;
    }
    if (config.user.empty() || config.password.empty() || config.database.empty()) {
        if (err != nullptr) {
            *err = "数据库配置不完整：db.user / db.password / db.name 不能为空";
        }
        return false;
    }

    auto state = std::make_shared<Impl>();
    state->config = config;
    state->target_size = config.pool_size;
    state->idle.reserve(config.pool_size);

    for (std::size_t i = 0; i < config.pool_size; ++i) {
        auto connection = std::make_unique<MySqlConnection>();
        std::string connection_error;
        if (!connection->connect(config, &connection_error)) {
            if (err != nullptr) {
                *err = "创建第 " + std::to_string(i + 1) + "/" +
                       std::to_string(config.pool_size) + " 条数据库连接失败：" +
                       connection_error;
            }
            return false;
        }
        state->idle.push_back(std::move(connection));
    }

    impl_ = std::move(state);
    return true;
}

ConnectionPool::Lease ConnectionPool::acquire(int timeout_ms, std::string* err) {
    if (impl_ == nullptr) {
        if (err != nullptr) {
            *err = "连接池未初始化";
        }
        return Lease();
    }

    std::shared_ptr<Impl> state = impl_;
    const int wait_ms = timeout_ms < 0 ? state->config.acquire_timeout_ms : timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(wait_ms < 0 ? 0 : wait_ms);

    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->closing && state->idle.empty()) {
        if (wait_ms <= 0) {
            if (err != nullptr) {
                *err = "获取数据库连接超时（池大小 " + std::to_string(state->target_size) + "）";
            }
            return Lease();
        }
        if (state->available.wait_until(lock, deadline) == std::cv_status::timeout &&
            state->idle.empty()) {
            if (err != nullptr) {
                *err = "获取数据库连接超时（池大小 " + std::to_string(state->target_size) + "）";
            }
            return Lease();
        }
    }
    if (state->closing) {
        if (err != nullptr) {
            *err = "连接池已关闭";
        }
        return Lease();
    }

    std::unique_ptr<MySqlConnection> connection = std::move(state->idle.back());
    state->idle.pop_back();
    lock.unlock();

    if (!connection->ping()) {
        std::string reconnect_error;
        LOG_WARN("数据库连接失效，正在重建连接");
        if (!connection->connect(state->config, &reconnect_error)) {
            if (err != nullptr) {
                *err = "数据库连接失效且重建失败：" + reconnect_error;
            }
            recycle(state, std::move(connection));
            return Lease();
        }
        LOG_INFO("数据库连接重建成功");
    }

    return Lease(std::move(state), std::move(connection));
}

bool ConnectionPool::healthCheck(std::string* err) {
    Lease lease = acquire(1500, err);
    if (!lease) {
        return false;
    }

    PreparedStatement statement(lease.connection(), "SELECT 1", err);
    if (!statement.valid()) {
        return false;
    }
    if (!statement.bind({}, err) || !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row || row.size() != 1 || !row[0].has_value() || *row[0] != "1") {
        if (err != nullptr) {
            *err = "数据库健康检查返回了意外结果";
        }
        return false;
    }
    return true;
}

std::size_t ConnectionPool::capacity() const {
    return impl_ == nullptr ? 0 : impl_->target_size;
}

std::size_t ConnectionPool::idleCount() const {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->idle.size();
}

void ConnectionPool::shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    std::vector<std::unique_ptr<MySqlConnection>> connections;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->closing = true;
        connections.swap(impl_->idle);
    }
    impl_->available.notify_all();
    impl_.reset();
}

}  // namespace db
}  // namespace cofinance
