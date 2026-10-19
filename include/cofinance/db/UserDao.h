#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/PreparedStatement.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

struct User {
    std::uint64_t id = 0;
    std::string username;
    std::string password_hash;
    std::string nickname;
    std::string avatar_url;
    int status = 0;
};

// P3 只提供最基础的读取/插入；注册、登录、会话属于 P4。
class UserDao : private NonCopyable {
public:
    explicit UserDao(MySqlConnection& connection);

    bool findById(std::uint64_t id, std::optional<User>* user, std::string* err);
    bool findByUsername(const std::string& username, std::optional<User>* user, std::string* err);
    bool insert(User& user, std::string* err);

private:
    bool readOne(const std::string& sql, const std::vector<SqlParam>& params,
                 std::optional<User>* user, std::string* err);

    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
