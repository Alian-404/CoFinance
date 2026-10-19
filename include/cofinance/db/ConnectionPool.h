#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "cofinance/db/MySqlConnection.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

// 固定大小的 MySQL 连接池。
// 借出和归还在互斥锁内完成；ping/重连等网络 IO 一律在锁外执行。
class ConnectionPool : private NonCopyable {
private:
    struct Impl;
    static void recycle(const std::shared_ptr<Impl>& state,
                        std::unique_ptr<MySqlConnection> connection);

public:
    class Lease : private NonCopyable {
    public:
        Lease() = default;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        MySqlConnection& connection();
        const MySqlConnection& connection() const;
        explicit operator bool() const;
        void release();

    private:
        friend class ConnectionPool;
        Lease(std::shared_ptr<Impl> state, std::unique_ptr<MySqlConnection> connection);

        std::shared_ptr<Impl> state_;
        std::unique_ptr<MySqlConnection> connection_;
    };

    ConnectionPool();
    ~ConnectionPool();

    // 先创建全部连接，任何一条失败都拒绝启动，避免服务带病运行。
    bool init(const MySqlConfig& config, std::string* err);

    // timeout_ms < 0 使用配置中的 acquire_timeout_ms；超时返回无效 Lease。
    Lease acquire(int timeout_ms, std::string* err);

    // 执行 SELECT 1，供 /api/health 使用。
    bool healthCheck(std::string* err);

    std::size_t capacity() const;
    std::size_t idleCount() const;
    void shutdown();

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace db
}  // namespace cofinance
