#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/UserDao.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
class ConnectionPool;
}
namespace service {

struct ServiceError {
    int http_status = 500;
    int code = 2001;
    std::string message = "数据库错误";
};

struct AuthContext {
    db::User user;
    std::string token_hash;
};

struct LoginResult {
    std::string token;
    std::string expires_at;
    db::User user;
};

class UserService : private NonCopyable {
public:
    explicit UserService(db::ConnectionPool& pool);

    bool registerUser(const std::string& username, const std::string& password,
                      const std::string& nickname, db::User* user, ServiceError* error);
    bool login(const std::string& username, const std::string& password, LoginResult* result,
               ServiceError* error);
    bool authenticate(const std::string& token, AuthContext* context, ServiceError* error);
    bool logout(const std::string& token, ServiceError* error);
    bool logoutAll(const AuthContext& context, ServiceError* error);

    bool getMe(std::uint64_t user_id, db::User* user, ServiceError* error);
    bool updateProfile(std::uint64_t user_id, const std::optional<std::string>& nickname,
                       const std::optional<std::string>& avatar_url, db::User* user,
                       ServiceError* error);
    bool changePassword(const AuthContext& context, const std::string& old_password,
                        const std::string& new_password, ServiceError* error);
    bool deleteAccount(const AuthContext& context, ServiceError* error);
    bool searchUsers(std::uint64_t requester_id, const std::string& query,
                     std::vector<db::User>* users, ServiceError* error);

private:
    db::ConnectionPool& pool_;
};

}  // namespace service
}  // namespace cofinance
