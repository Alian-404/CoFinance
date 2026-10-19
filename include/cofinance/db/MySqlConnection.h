#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cofinance/util/Common.h"

struct MYSQL;

namespace cofinance {
namespace db {

struct MySqlConfig {
    std::string host = "127.0.0.1";
    unsigned int port = 3306;
    std::string user;
    std::string password;
    std::string database;
    std::size_t pool_size = 16;
    int connect_timeout_ms = 3000;
    int acquire_timeout_ms = 3000;
};

// 一条 MySQL 连接。构造/析构不碰网络，connect() 才真正建立连接。
// 所有方法都由持有它的线程调用；连接池保证同一时刻至多一个借用者。
class MySqlConnection : private NonCopyable {
public:
    MySqlConnection();
    ~MySqlConnection();

    bool connect(const MySqlConfig& config, std::string* err);
    void close();
    bool ping() const;
    bool connected() const;

    // 仅用于事务控制、SET 等不含用户输入的小语句。业务 SQL 必须走
    // PreparedStatement，绝不把参数拼进 SQL 字符串。
    bool execute(const std::string& sql, std::string* err);

    std::uint64_t affectedRows() const;
    std::uint64_t lastInsertId() const;
    const std::string& lastError() const;

    MYSQL* nativeHandle() const;

private:
    void setError(const char* prefix);

    MYSQL* mysql_ = nullptr;
    std::string last_error_;
};

}  // namespace db
}  // namespace cofinance
