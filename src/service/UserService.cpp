#include "cofinance/service/UserService.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/SessionDao.h"
#include "cofinance/db/TransactionGuard.h"
#include "cofinance/util/Crypto.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {
namespace {

void setError(ServiceError* error, int status, int code, std::string message) {
    if (error != nullptr) {
        error->http_status = status;
        error->code = code;
        error->message = std::move(message);
    }
}

void setDatabaseError(ServiceError* error, const char* operation, const std::string& detail) {
    LOG_ERROR("%s 失败：%s", operation, detail.c_str());
    setError(error, 500, 2001, "服务器内部错误");
}

bool hasControlCharacters(const std::string& text) {
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            return true;
        }
    }
    return false;
}

bool validUsername(const std::string& username) {
    if (username.size() < 4 || username.size() > 20) {
        return false;
    }
    for (char c : username) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool validPassword(const std::string& password) {
    return password.size() >= 8 && password.size() <= 128;
}

bool validNickname(const std::string& nickname) {
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(nickname, &valid_utf8);
    return valid_utf8 && length <= 50 && !hasControlCharacters(nickname);
}

bool validAvatarUrl(const std::string& avatar_url) {
    bool valid_utf8 = false;
    util::utf8Length(avatar_url, &valid_utf8);
    if (!valid_utf8 || avatar_url.size() > 255 || hasControlCharacters(avatar_url)) {
        return false;
    }
    if (avatar_url.empty()) {
        return true;
    }
    return avatar_url.front() == '/' && avatar_url.rfind("//", 0) != 0 &&
           avatar_url.find("..") == std::string::npos;
}

}  // namespace

UserService::UserService(db::ConnectionPool& pool) : pool_(pool) {}

bool UserService::registerUser(const std::string& username, const std::string& password,
                               const std::string& nickname, db::User* user,
                               ServiceError* error) {
    if (user == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (!validUsername(username)) {
        setError(error, 400, 1001, "用户名须为 4-20 位字母、数字或下划线");
        return false;
    }
    if (!validPassword(password)) {
        setError(error, 400, 1001, "密码长度须为 8-128 字节");
        return false;
    }
    if (!validNickname(nickname)) {
        setError(error, 400, 1001, "昵称不能超过 50 个字符且不能包含控制字符");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::UserDao dao(lease.connection());
    std::optional<db::User> existing;
    if (!dao.findByUsername(username, &existing, &db_error)) {
        setDatabaseError(error, "查询用户名", db_error);
        return false;
    }
    if (existing.has_value()) {
        setError(error, 409, 1006, "用户名已存在");
        return false;
    }

    std::string password_hash;
    if (!util::hashPassword(password, &password_hash, &db_error)) {
        setDatabaseError(error, "生成密码哈希", db_error);
        return false;
    }

    db::User created;
    created.username = username;
    created.password_hash = password_hash;
    created.nickname = nickname.empty() ? username : nickname;
    bool duplicate = false;
    if (!dao.insert(created, &db_error, &duplicate)) {
        if (duplicate) {
            setError(error, 409, 1006, "用户名已存在");
        } else {
            setDatabaseError(error, "创建用户", db_error);
        }
        return false;
    }
    created.password_hash.clear();
    *user = std::move(created);
    return true;
}

bool UserService::login(const std::string& username, const std::string& password,
                        LoginResult* result, ServiceError* error) {
    if (result == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (username.empty() || username.size() > 50 || password.empty() || password.size() > 128) {
        setError(error, 400, 1001, "用户名和密码不能为空");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::UserDao dao(lease.connection());
    std::optional<db::User> user;
    if (!dao.findActiveByUsername(username, &user, &db_error)) {
        setDatabaseError(error, "查询登录用户", db_error);
        return false;
    }
    if (!user.has_value()) {
        util::burnPasswordVerification(password);
        setError(error, 401, 1007, "用户名或密码错误");
        return false;
    }

    std::string verify_error;
    if (!util::verifyPassword(password, user->password_hash, &verify_error)) {
        if (!verify_error.empty()) {
            setDatabaseError(error, "校验密码哈希", verify_error);
        } else {
            setError(error, 401, 1007, "用户名或密码错误");
        }
        return false;
    }

    std::string token;
    if (!util::randomHex(32, &token, &db_error)) {
        setDatabaseError(error, "生成会话 token", db_error);
        return false;
    }
    const std::string token_hash = util::sha256Hex(token);
    if (token_hash.size() != 64) {
        setDatabaseError(error, "计算 token 哈希", "SHA256 输出长度异常");
        return false;
    }
    db::SessionDao sessions(lease.connection());
    if (!sessions.create(user->id, token_hash, &db_error)) {
        setDatabaseError(error, "创建登录会话", db_error);
        return false;
    }

    LoginResult out;
    out.token = token;
    out.expires_at = util::formatUtcMillis(util::nowUtcMillis() + 7LL * 24 * 3600 * 1000);
    out.user = *user;
    out.user.password_hash.clear();
    *result = std::move(out);
    return true;
}

bool UserService::authenticate(const std::string& token, AuthContext* context,
                               ServiceError* error) {
    if (context == nullptr || token.empty() || token.size() > 256) {
        setError(error, 401, 1002, "未登录或 token 无效");
        return false;
    }
    const std::string token_hash = util::sha256Hex(token);
    if (token_hash.empty()) {
        setDatabaseError(error, "计算 token 哈希", "SHA256 计算失败");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::SessionDao sessions(lease.connection());
    std::optional<db::SessionRecord> session;
    if (!sessions.find(token_hash, &session, &db_error)) {
        setDatabaseError(error, "查询登录会话", db_error);
        return false;
    }
    if (!session.has_value()) {
        setError(error, 401, 1002, "未登录或 token 无效");
        return false;
    }
    if (session->expired) {
        bool removed = false;
        if (!sessions.remove(token_hash, &removed, &db_error)) {
            LOG_WARN("删除过期会话失败：%s", db_error.c_str());
        }
        setError(error, 401, 1003, "登录已过期，请重新登录");
        return false;
    }

    db::UserDao users(lease.connection());
    std::optional<db::User> user;
    if (!users.findActiveById(session->user_id, &user, &db_error)) {
        setDatabaseError(error, "读取当前用户", db_error);
        return false;
    }
    if (!user.has_value()) {
        bool removed = false;
        sessions.remove(token_hash, &removed, &db_error);
        setError(error, 401, 1002, "账号不存在或已注销");
        return false;
    }

    if (session->needs_renewal) {
        bool touched = false;
        if (!sessions.touch(token_hash, &touched, &db_error)) {
            setDatabaseError(error, "续期登录会话", db_error);
            return false;
        }
        if (!touched) {
            setError(error, 401, 1003, "登录已过期，请重新登录");
            return false;
        }
    }

    context->user = *user;
    context->token_hash = token_hash;
    return true;
}

bool UserService::logout(const std::string& token, ServiceError* error) {
    if (token.empty() || token.size() > 256) {
        setError(error, 401, 1002, "未登录或 token 无效");
        return false;
    }
    const std::string token_hash = util::sha256Hex(token);
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::SessionDao sessions(lease.connection());
    bool removed = false;
    if (!sessions.remove(token_hash, &removed, &db_error)) {
        setDatabaseError(error, "删除登录会话", db_error);
        return false;
    }
    if (!removed) {
        setError(error, 401, 1002, "未登录或 token 无效");
        return false;
    }
    return true;
}

bool UserService::logoutAll(const AuthContext& context, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::SessionDao sessions(lease.connection());
    if (!sessions.removeAll(context.user.id, &db_error)) {
        setDatabaseError(error, "吊销全部会话", db_error);
        return false;
    }
    return true;
}

bool UserService::getMe(std::uint64_t user_id, db::User* user, ServiceError* error) {
    if (user == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::UserDao dao(lease.connection());
    std::optional<db::User> found;
    if (!dao.findActiveById(user_id, &found, &db_error)) {
        setDatabaseError(error, "读取当前用户", db_error);
        return false;
    }
    if (!found.has_value()) {
        setError(error, 404, 1005, "用户不存在");
        return false;
    }
    found->password_hash.clear();
    *user = std::move(*found);
    return true;
}

bool UserService::updateProfile(std::uint64_t user_id,
                                const std::optional<std::string>& nickname,
                                const std::optional<std::string>& avatar_url, db::User* user,
                                ServiceError* error) {
    if (user == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (!nickname.has_value() && !avatar_url.has_value()) {
        setError(error, 400, 1001, "至少提供 nickname 或 avatar_url");
        return false;
    }
    if (nickname.has_value() && !validNickname(*nickname)) {
        setError(error, 400, 1001, "昵称不能超过 50 个字符且不能包含控制字符");
        return false;
    }
    if (avatar_url.has_value() && !validAvatarUrl(*avatar_url)) {
        setError(error, 400, 1001, "头像地址不能超过 255 字节且不能包含控制字符");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::UserDao dao(lease.connection());
    std::optional<db::User> current;
    if (!dao.findActiveById(user_id, &current, &db_error)) {
        setDatabaseError(error, "读取用户资料", db_error);
        return false;
    }
    if (!current.has_value()) {
        setError(error, 404, 1005, "用户不存在");
        return false;
    }
    const std::string next_nickname = nickname.value_or(current->nickname);
    const std::string next_avatar = avatar_url.value_or(current->avatar_url);
    if (!dao.updateProfile(user_id, next_nickname, next_avatar, &db_error)) {
        setDatabaseError(error, "更新用户资料", db_error);
        return false;
    }
    current->nickname = next_nickname;
    current->avatar_url = next_avatar;
    current->password_hash.clear();
    *user = std::move(*current);
    return true;
}

bool UserService::changePassword(const AuthContext& context, const std::string& old_password,
                                 const std::string& new_password, ServiceError* error) {
    if (old_password.empty() || old_password.size() > 128) {
        setError(error, 400, 1001, "旧密码长度非法");
        return false;
    }
    if (!validPassword(new_password)) {
        setError(error, 400, 1001, "新密码长度须为 8-128 字节");
        return false;
    }
    std::string verify_error;
    if (!util::verifyPassword(old_password, context.user.password_hash, &verify_error)) {
        if (!verify_error.empty()) {
            setDatabaseError(error, "校验旧密码哈希", verify_error);
        } else {
            setError(error, 401, 1007, "用户名或密码错误");
        }
        return false;
    }

    std::string new_hash;
    if (!util::hashPassword(new_password, &new_hash, &verify_error)) {
        setDatabaseError(error, "生成新密码哈希", verify_error);
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启修改密码事务", db_error);
        return false;
    }
    db::UserDao users(lease.connection());
    bool updated = false;
    if (!users.updatePasswordHashIfCurrent(context.user.id, context.user.password_hash, new_hash,
                                           &updated, &db_error)) {
        setDatabaseError(error, "更新密码哈希", db_error);
        return false;
    }
    if (!updated) {
        setError(error, 401, 1007, "密码已被修改，请重新登录后再试");
        return false;
    }
    db::SessionDao sessions(lease.connection());
    if (!sessions.removeOthers(context.user.id, context.token_hash, &db_error)) {
        setDatabaseError(error, "吊销其它会话", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交修改密码事务", db_error);
        return false;
    }
    return true;
}

bool UserService::deleteAccount(const AuthContext& context, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启注销事务", db_error);
        return false;
    }
    db::UserDao users(lease.connection());
    if (!users.softDelete(context.user.id, &db_error)) {
        setDatabaseError(error, "软删除用户", db_error);
        return false;
    }
    db::SessionDao sessions(lease.connection());
    if (!sessions.removeAll(context.user.id, &db_error)) {
        setDatabaseError(error, "吊销注销用户的会话", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交注销事务", db_error);
        return false;
    }
    return true;
}

bool UserService::searchUsers(std::uint64_t requester_id, const std::string& query,
                              std::vector<db::User>* users, ServiceError* error) {
    if (users == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(query, &valid_utf8);
    if (!valid_utf8 || length == 0 || length > 50 || hasControlCharacters(query)) {
        setError(error, 400, 1001, "搜索词长度须为 1-50 个字符");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::UserDao dao(lease.connection());
    if (!dao.searchActiveByUsername(query, requester_id, users, &db_error)) {
        setDatabaseError(error, "搜索用户", db_error);
        return false;
    }
    for (db::User& user : *users) {
        user.password_hash.clear();
    }
    return true;
}

}  // namespace service
}  // namespace cofinance
