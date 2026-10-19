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

// 用户表的数据访问。业务规则（格式校验、鉴权、会话）在 UserService。
class UserDao : private NonCopyable {
public:
    explicit UserDao(MySqlConnection& connection);

    bool findById(std::uint64_t id, std::optional<User>* user, std::string* err);
    bool findByUsername(const std::string& username, std::optional<User>* user, std::string* err);
    bool findActiveById(std::uint64_t id, std::optional<User>* user, std::string* err);
    bool findActiveByUsername(const std::string& username, std::optional<User>* user,
                              std::string* err);
    bool lockActiveById(std::uint64_t id, std::optional<User>* user, std::string* err);
    bool lockActiveByUsername(const std::string& username, std::optional<User>* user,
                              std::string* err);
    bool insert(User& user, std::string* err, bool* duplicate = nullptr);
    bool updateProfile(std::uint64_t id, const std::string& nickname,
                       const std::string& avatar_url, std::string* err);
    bool updatePasswordHashIfCurrent(std::uint64_t id, const std::string& expected_hash,
                                     const std::string& new_hash, bool* updated,
                                     std::string* err);
    bool softDelete(std::uint64_t id, std::string* err);
    bool searchActiveByUsername(const std::string& query, std::uint64_t exclude_id,
                                std::vector<User>* users, std::string* err);

private:
    bool readOne(const std::string& sql, const std::vector<SqlParam>& params,
                 std::optional<User>* user, std::string* err);

    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
