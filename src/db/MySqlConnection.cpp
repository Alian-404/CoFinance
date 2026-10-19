#include "cofinance/db/MySqlConnection.h"

#include <mysql/mysql.h>

#include <mutex>
#include <string>

namespace cofinance {
namespace db {
namespace {

void ensureLibraryInitialized() {
    static std::once_flag once;
    std::call_once(once, []() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            // 后续 mysql_init 会给出更具体的错误；这里不抛出，避免静态初始化期异常。
        }
    });
}

void ensureThreadInitialized() {
    struct ThreadGuard {
        ThreadGuard() : ok(mysql_thread_init()) {}
        ~ThreadGuard() {
            if (ok) {
                mysql_thread_end();
            }
        }
        bool ok;
    };
    thread_local ThreadGuard guard;
    (void)guard;
}

unsigned int secondsFromMillis(int ms) {
    if (ms <= 0) {
        return 1;
    }
    return static_cast<unsigned int>((ms + 999) / 1000);
}

}  // namespace

MySqlConnection::MySqlConnection() = default;

MySqlConnection::~MySqlConnection() {
    close();
}

bool MySqlConnection::connect(const MySqlConfig& config, std::string* err) {
    close();
    ensureLibraryInitialized();
    ensureThreadInitialized();

    mysql_ = mysql_init(nullptr);
    if (mysql_ == nullptr) {
        last_error_ = "mysql_init 失败（内存不足）";
        if (err != nullptr) {
            *err = last_error_;
        }
        return false;
    }

    const unsigned int timeout = secondsFromMillis(config.connect_timeout_ms);
    mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(mysql_, config.host.c_str(), config.user.c_str(),
                           config.password.c_str(), config.database.c_str(), config.port,
                           nullptr, CLIENT_FOUND_ROWS) == nullptr) {
        setError("连接 MySQL 失败");
        close();
        if (err != nullptr) {
            *err = last_error_;
        }
        return false;
    }

    // DATETIME 的写入/读取统一使用 UTC，避免服务器本地时区参与业务语义。
    const char kSetUtcTimeZone[] = "SET time_zone = '+00:00'";
    if (mysql_real_query(mysql_, kSetUtcTimeZone,
                         static_cast<unsigned long>(sizeof(kSetUtcTimeZone) - 1)) != 0) {
        setError("设置 MySQL 时区失败");
        close();
        if (err != nullptr) {
            *err = last_error_;
        }
        return false;
    }

    last_error_.clear();
    return true;
}

void MySqlConnection::close() {
    if (mysql_ != nullptr) {
        ensureThreadInitialized();
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
}

bool MySqlConnection::ping() const {
    ensureThreadInitialized();
    return mysql_ != nullptr && mysql_ping(mysql_) == 0;
}

bool MySqlConnection::connected() const {
    return mysql_ != nullptr;
}

bool MySqlConnection::execute(const std::string& sql, std::string* err) {
    ensureThreadInitialized();
    if (mysql_ == nullptr) {
        last_error_ = "连接未建立";
        if (err != nullptr) {
            *err = last_error_;
        }
        return false;
    }
    if (mysql_real_query(mysql_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        setError("执行 SQL 失败");
        if (err != nullptr) {
            *err = last_error_;
        }
        return false;
    }
    return true;
}

std::uint64_t MySqlConnection::affectedRows() const {
    return mysql_ == nullptr ? 0 : static_cast<std::uint64_t>(mysql_affected_rows(mysql_));
}

std::uint64_t MySqlConnection::lastInsertId() const {
    return mysql_ == nullptr ? 0 : static_cast<std::uint64_t>(mysql_insert_id(mysql_));
}

const std::string& MySqlConnection::lastError() const {
    return last_error_;
}

MYSQL* MySqlConnection::nativeHandle() const {
    return mysql_;
}

void MySqlConnection::setError(const char* prefix) {
    const unsigned int code = mysql_ == nullptr ? 0 : mysql_errno(mysql_);
    const char* detail = mysql_ == nullptr ? "未知错误" : mysql_error(mysql_);
    last_error_ = std::string(prefix) + "（errno=" + std::to_string(code) + "）：" + detail;
}

}  // namespace db
}  // namespace cofinance
