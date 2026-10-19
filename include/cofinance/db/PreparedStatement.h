#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

using SqlParam = std::variant<std::nullptr_t, std::string, std::int64_t, std::uint64_t, double>;
using SqlRow = std::vector<std::optional<std::string>>;

// MySQL 服务端预处理语句。问号参数通过二进制协议发送，值不会被当成 SQL 语法，
// 这是防注入的根本手段；不要用字符串拼接代替它。
class PreparedStatement : private NonCopyable {
public:
    PreparedStatement(MySqlConnection& connection, const std::string& sql, std::string* err);
    ~PreparedStatement();

    PreparedStatement(PreparedStatement&&) = delete;
    PreparedStatement& operator=(PreparedStatement&&) = delete;

    bool valid() const;
    bool bind(const std::vector<SqlParam>& params, std::string* err);
    bool execute(std::string* err);
    bool fetch(SqlRow* row, bool* has_row, std::string* err);

    std::uint64_t affectedRows() const;
    std::uint64_t lastInsertId() const;
    unsigned int columnCount() const;
    unsigned int lastErrorCode() const;
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace db
}  // namespace cofinance
