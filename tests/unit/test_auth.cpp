#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/PreparedStatement.h"
#include "cofinance/db/UserDao.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Config.h"
#include "cofinance/util/Crypto.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                            \
    } while (false)

std::string siblingPath(const std::string& path, const std::string& name) {
    return (std::filesystem::path(path).parent_path() / name).string();
}

bool loadSchema(const std::string& path, cofinance::db::MySqlConnection& connection,
                std::string* err) {
    std::ifstream input(path);
    if (!input) {
        *err = "无法读取 " + path;
        return false;
    }
    std::string cleaned;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line.compare(first, 2, "--") == 0) {
            continue;
        }
        cleaned += line;
        cleaned.push_back('\n');
    }

    std::string statement;
    for (char c : cleaned) {
        if (c != ';') {
            statement.push_back(c);
            continue;
        }
        if (statement.find_first_not_of(" \t\r\n") != std::string::npos) {
            if (!connection.execute(statement, err)) {
                return false;
            }
        }
        statement.clear();
    }
    return true;
}

bool queryScalar(cofinance::db::MySqlConnection& connection, const std::string& sql,
                 const std::vector<cofinance::db::SqlParam>& params, std::string* value,
                 std::string* err) {
    cofinance::db::PreparedStatement statement(connection, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    cofinance::db::SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err) || !has_row || row.size() != 1 ||
        !row[0].has_value()) {
        if (err->empty()) {
            *err = "标量查询没有返回一行一列";
        }
        return false;
    }
    *value = *row[0];
    return true;
}

std::string uniqueName(const std::string& tag, int suffix) {
    const long long pid = static_cast<long long>(::getpid()) % 100000;
    const long long now = cofinance::util::nowUtcMillis() % 1000000;
    return "p4" + tag.substr(0, 1) + std::to_string(pid) + std::to_string(now) +
           std::to_string(suffix);
}

cofinance::db::MySqlConfig makeConfig(const cofinance::Config& cfg) {
    cofinance::db::MySqlConfig out;
    out.host = cfg.get("db.host", "127.0.0.1");
    out.port = static_cast<unsigned int>(cfg.getInt("db.port", 3306));
    out.user = cfg.get("db.user");
    out.password = cfg.get("db.password");
    out.database = cfg.get("db.name", "cofinance");
    out.pool_size = 4;
    out.connect_timeout_ms = static_cast<int>(cfg.getInt("db.connect_timeout_ms", 3000));
    out.acquire_timeout_ms = static_cast<int>(cfg.getInt("db.acquire_timeout_ms", 3000));
    return out;
}

void hardDeleteUser(cofinance::db::MySqlConnection& connection, std::uint64_t user_id) {
    std::string err;
    cofinance::db::PreparedStatement sessions(connection,
                                               "DELETE FROM sessions WHERE user_id = ?", &err);
    if (sessions.valid()) {
        sessions.bind({cofinance::db::SqlParam{user_id}}, &err);
        sessions.execute(&err);
    }
    cofinance::db::PreparedStatement users(connection, "DELETE FROM users WHERE id = ?", &err);
    if (users.valid()) {
        users.bind({cofinance::db::SqlParam{user_id}}, &err);
        users.execute(&err);
    }
}

void testCrypto() {
    std::printf("[case] PBKDF2、SHA256、随机 token\n");
    std::string err;
    std::string first;
    std::string second;
    CHECK(cofinance::util::hashPassword("Password123", &first, &err));
    CHECK(cofinance::util::hashPassword("Password123", &second, &err));
    CHECK(first != second);
    CHECK(first.rfind("pbkdf2_sha256$100000$", 0) == 0);
    CHECK(cofinance::util::verifyPassword("Password123", first, &err));
    CHECK(err.empty());
    CHECK(!cofinance::util::verifyPassword("WrongPassword", first, &err));
    CHECK(err.empty());
    CHECK(!cofinance::util::verifyPassword("Password123", "broken", &err));
    CHECK(!err.empty());
    CHECK(cofinance::util::sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string token1;
    std::string token2;
    CHECK(cofinance::util::randomHex(32, &token1, &err));
    CHECK(cofinance::util::randomHex(32, &token2, &err));
    CHECK(token1.size() == 64);
    CHECK(token1 != token2);
    bool valid_utf8 = false;
    CHECK(cofinance::util::utf8Length("你好", &valid_utf8) == 2 && valid_utf8);
    CHECK(cofinance::util::utf8Length(std::string("\xff", 1), &valid_utf8) == 0 && !valid_utf8);
}

void testUserService(cofinance::db::ConnectionPool& pool, const std::string& schema_path) {
    std::printf("[case] 注册、登录、会话、改密与注销\n");
    std::string err;
    auto setup = pool.acquire(1000, &err);
    CHECK(static_cast<bool>(setup));
    if (!setup) {
        return;
    }
    CHECK(loadSchema(schema_path, setup.connection(), &err));

    cofinance::service::UserService service(pool);
    cofinance::service::ServiceError error;
    const std::string username = uniqueName("alice", 0);
    const std::string second_name = uniqueName("bob", 1);
    CHECK(username.size() <= 20 && second_name.size() <= 20);

    cofinance::db::User alice;
    CHECK(service.registerUser(username, "Password123", "Alice", &alice, &error));
    CHECK(alice.id > 0);
    CHECK(alice.password_hash.empty());

    CHECK(!service.registerUser(username, "Password123", "Alice", &alice, &error));
    CHECK(error.code == 1006 && error.http_status == 409);
    CHECK(!service.registerUser("ab", "Password123", "", &alice, &error));
    CHECK(error.code == 1001);
    CHECK(!service.registerUser(uniqueName("bad", 2), "short", "", &alice, &error));
    CHECK(error.code == 1001);

    cofinance::service::LoginResult login;
    CHECK(!service.login(username, "WrongPassword", &login, &error));
    CHECK(error.code == 1007 && error.http_status == 401);
    CHECK(service.login(username, "Password123", &login, &error));
    CHECK(login.token.size() == 64);
    CHECK(!login.expires_at.empty() && login.expires_at.back() == 'Z');
    CHECK(login.user.id == alice.id);

    const std::string token_hash = cofinance::util::sha256Hex(login.token);
    std::string count;
    CHECK(queryScalar(setup.connection(),
                      "SELECT password_hash FROM users WHERE id = ?",
                      {cofinance::db::SqlParam{alice.id}}, &count, &err));
    CHECK(count.rfind("pbkdf2_sha256$", 0) == 0);
    CHECK(count != "Password123");
    CHECK(queryScalar(setup.connection(),
                      "SELECT COUNT(*) FROM sessions WHERE token_hash = ? AND user_id = ?",
                      {cofinance::db::SqlParam{token_hash},
                       cofinance::db::SqlParam{alice.id}},
                      &count, &err));
    CHECK(count == "1");
    CHECK(queryScalar(setup.connection(),
                      "SELECT COUNT(*) FROM sessions WHERE token_hash = ?",
                      {cofinance::db::SqlParam{login.token}}, &count, &err));
    CHECK(count == "0");

    cofinance::service::AuthContext context;
    CHECK(service.authenticate(login.token, &context, &error));
    CHECK(context.user.id == alice.id);
    CHECK(context.token_hash == token_hash);

    cofinance::db::User me;
    CHECK(service.getMe(alice.id, &me, &error));
    CHECK(me.username == username);
    CHECK(service.updateProfile(alice.id, std::string("Alice New"), std::string("/avatars/1.png"),
                                &me, &error));
    CHECK(me.nickname == "Alice New" && me.avatar_url == "/avatars/1.png");
    CHECK(service.updateProfile(alice.id, std::string("Alice New"), std::string("/avatars/1.png"),
                                &me, &error));

    cofinance::db::User bob;
    CHECK(service.registerUser(second_name, "Password456", "Bob", &bob, &error));
    cofinance::service::LoginResult bob_login;
    CHECK(service.login(second_name, "Password456", &bob_login, &error));
    std::vector<cofinance::db::User> found;
    CHECK(service.searchUsers(bob.id, username, &found, &error));
    CHECK(found.size() == 1 && found[0].id == alice.id);
    CHECK(service.searchUsers(bob.id, "%", &found, &error));
    CHECK(found.empty());

    cofinance::service::LoginResult second_login;
    CHECK(service.login(username, "Password123", &second_login, &error));
    CHECK(service.changePassword(context, "Password123", "NewPassword456", &error));
    CHECK(service.authenticate(login.token, &context, &error));
    CHECK(!service.authenticate(second_login.token, &context, &error));
    CHECK(error.code == 1002);
    CHECK(!service.login(username, "Password123", &login, &error));
    CHECK(error.code == 1007);

    cofinance::service::LoginResult fresh;
    CHECK(service.login(username, "NewPassword456", &fresh, &error));
    CHECK(service.logout(login.token, &error));
    CHECK(!service.authenticate(login.token, &context, &error));
    CHECK(error.code == 1002);
    CHECK(service.authenticate(fresh.token, &context, &error));
    CHECK(service.logoutAll(context, &error));
    CHECK(!service.authenticate(fresh.token, &context, &error));

    std::string expired_token;
    CHECK(cofinance::util::randomHex(32, &expired_token, &err));
    const std::string expired_hash = cofinance::util::sha256Hex(expired_token);
    cofinance::db::PreparedStatement expired(
        setup.connection(),
        "INSERT INTO sessions (token_hash, user_id, expires_at, last_seen_at) "
        "VALUES (?, ?, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 SECOND), UTC_TIMESTAMP())",
        &err);
    CHECK(expired.valid());
    CHECK(expired.bind({cofinance::db::SqlParam{expired_hash},
                        cofinance::db::SqlParam{alice.id}}, &err));
    CHECK(expired.execute(&err));
    CHECK(!service.authenticate(expired_token, &context, &error));
    CHECK(error.code == 1003 && error.http_status == 401);
    CHECK(queryScalar(setup.connection(),
                      "SELECT COUNT(*) FROM sessions WHERE token_hash = ?",
                      {cofinance::db::SqlParam{expired_hash}}, &count, &err));
    CHECK(count == "0");

    cofinance::service::LoginResult doomed;
    CHECK(service.login(username, "NewPassword456", &doomed, &error));
    CHECK(service.authenticate(doomed.token, &context, &error));
    CHECK(service.deleteAccount(context, &error));
    CHECK(!service.authenticate(doomed.token, &context, &error));
    CHECK(!service.login(username, "NewPassword456", &login, &error));
    CHECK(error.code == 1007);
    CHECK(!service.registerUser(username, "Password123", "", &alice, &error));
    CHECK(error.code == 1006);

    hardDeleteUser(setup.connection(), alice.id);
    hardDeleteUser(setup.connection(), bob.id);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";

    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err) || !cfg.loadFile(siblingPath(config_path, "secrets.conf"),
                                                          &err)) {
        std::fprintf(stderr, "P4 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    testCrypto();

    cofinance::db::ConnectionPool pool;
    if (!pool.init(makeConfig(cfg), &err)) {
        std::fprintf(stderr, "P4 测试连接池初始化失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    testUserService(pool, schema_path);

    if (g_failed != 0) {
        std::fprintf(stderr, "P4 用户系统测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P4 用户系统测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
