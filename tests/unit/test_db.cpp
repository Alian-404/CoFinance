#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/PreparedStatement.h"
#include "cofinance/db/TransactionGuard.h"
#include "cofinance/db/UserDao.h"
#include "cofinance/util/Config.h"

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
    const std::filesystem::path base(path);
    return (base.parent_path() / name).string();
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
        std::size_t first = line.find_first_not_of(" \t\r");
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
        const std::size_t first = statement.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            if (!connection.execute(statement, err)) {
                return false;
            }
        }
        statement.clear();
    }
    return true;
}

bool queryRow(cofinance::db::MySqlConnection& connection, const std::string& sql,
              const std::vector<cofinance::db::SqlParam>& params,
              cofinance::db::SqlRow* row, bool* has_row, std::string* err) {
    cofinance::db::PreparedStatement statement(connection, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
        return false;
    }
    return statement.fetch(row, has_row, err);
}

bool queryScalar(cofinance::db::MySqlConnection& connection, const std::string& sql,
                 const std::vector<cofinance::db::SqlParam>& params, std::string* value,
                 std::string* err) {
    cofinance::db::SqlRow row;
    bool has_row = false;
    if (!queryRow(connection, sql, params, &row, &has_row, err)) {
        return false;
    }
    if (!has_row || row.size() != 1 || !row[0].has_value()) {
        *err = "标量查询没有返回一行一列";
        return false;
    }
    *value = *row[0];
    return true;
}

void deleteUser(cofinance::db::MySqlConnection& connection, std::uint64_t id) {
    cofinance::db::PreparedStatement statement(connection, "DELETE FROM users WHERE id = ?",
                                                nullptr);
    std::string err;
    if (statement.valid()) {
        statement.bind({cofinance::db::SqlParam{id}}, &err);
        statement.execute(&err);
    }
}

std::string uniqueName(const std::string& tag, int suffix) {
    return "p3_" + tag + "_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
           std::to_string(cofinance::util::nowUtcMillis()) + "_" + std::to_string(suffix);
}

cofinance::db::MySqlConfig makeConfig(const cofinance::Config& cfg, std::size_t pool_size) {
    cofinance::db::MySqlConfig out;
    out.host = cfg.get("db.host", "127.0.0.1");
    out.port = static_cast<unsigned int>(cfg.getInt("db.port", 3306));
    out.user = cfg.get("db.user");
    out.password = cfg.get("db.password");
    out.database = cfg.get("db.name", "cofinance");
    out.pool_size = pool_size;
    out.connect_timeout_ms = static_cast<int>(cfg.getInt("db.connect_timeout_ms", 3000));
    out.acquire_timeout_ms = static_cast<int>(cfg.getInt("db.acquire_timeout_ms", 3000));
    return out;
}

void testConnectionAndSchema(const cofinance::db::MySqlConfig& config,
                             const std::string& schema_path) {
    std::printf("[case] 连接参数与 schema 幂等加载\n");
    cofinance::db::ConnectionPool pool;
    std::string err;
    CHECK(pool.init(config, &err));
    CHECK(pool.capacity() == config.pool_size);
    CHECK(pool.healthCheck(&err));

    auto lease = pool.acquire(1000, &err);
    CHECK(static_cast<bool>(lease));
    if (!lease) {
        std::fprintf(stderr, "    %s\n", err.c_str());
        return;
    }

    cofinance::db::SqlRow row;
    bool has_row = false;
    CHECK(queryRow(lease.connection(), "SELECT @@character_set_client, @@time_zone", {}, &row,
                   &has_row, &err));
    CHECK(has_row);
    CHECK(row.size() == 2);
    if (row.size() == 2) {
        CHECK(row[0].value_or("") == "utf8mb4");
        CHECK(row[1].value_or("") == "+00:00");
    }

    CHECK(loadSchema(schema_path, lease.connection(), &err));
    CHECK(loadSchema(schema_path, lease.connection(), &err));
    std::string table_count;
    CHECK(queryScalar(
        lease.connection(),
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
        "AND table_name IN ('users','finance_groups','group_members','categories',"
        "'transactions','transaction_participants','messages','sessions','settlements')",
        {}, &table_count, &err));
    CHECK(table_count == "9");
}

void testPreparedStatementAndInjection(cofinance::db::ConnectionPool& pool) {
    std::printf("[case] 预处理语句与注入串\n");
    std::string err;
    auto lease = pool.acquire(1000, &err);
    CHECK(static_cast<bool>(lease));
    if (!lease) {
        return;
    }

    cofinance::db::UserDao dao(lease.connection());
    cofinance::db::User normal;
    normal.username = uniqueName("normal", 0);
    normal.password_hash = "hash-normal";
    normal.nickname = "普通用户";
    CHECK(dao.insert(normal, &err));
    CHECK(normal.id > 0);

    cofinance::db::User injected;
    injected.username = "p3_x' OR '1'='1";
    injected.password_hash = "hash-injected";
    injected.nickname = "注入串只是数据";
    CHECK(dao.insert(injected, &err));

    std::optional<cofinance::db::User> found;
    CHECK(dao.findByUsername(injected.username, &found, &err));
    CHECK(found.has_value());
    CHECK(found.has_value() && found->id == injected.id);
    CHECK(found.has_value() && found->nickname == injected.nickname);

    found.reset();
    CHECK(dao.findByUsername("' OR 1=1 -- ", &found, &err));
    CHECK(!found.has_value());

    found.reset();
    CHECK(dao.findById(normal.id, &found, &err));
    CHECK(found.has_value() && found->username == normal.username);

    deleteUser(lease.connection(), normal.id);
    deleteUser(lease.connection(), injected.id);
}

void testTransactionGuard(cofinance::db::ConnectionPool& pool) {
    std::printf("[case] 事务提交与析构回滚\n");
    std::string err;
    auto lease = pool.acquire(1000, &err);
    CHECK(static_cast<bool>(lease));
    if (!lease) {
        return;
    }

    cofinance::db::UserDao dao(lease.connection());
    cofinance::db::User rolled_back;
    rolled_back.username = uniqueName("rollback", 1);
    rolled_back.password_hash = "hash-rb";
    {
        cofinance::db::TransactionGuard tx(lease.connection(), &err);
        CHECK(tx.active());
        CHECK(dao.insert(rolled_back, &err));
    }

    std::optional<cofinance::db::User> found;
    CHECK(dao.findByUsername(rolled_back.username, &found, &err));
    CHECK(!found.has_value());

    cofinance::db::User committed;
    committed.username = uniqueName("commit", 2);
    committed.password_hash = "hash-commit";
    {
        cofinance::db::TransactionGuard tx(lease.connection(), &err);
        CHECK(tx.active());
        CHECK(dao.insert(committed, &err));
        CHECK(tx.commit(&err));
        CHECK(!tx.active());
        CHECK(!tx.commit(&err));
    }
    CHECK(dao.findByUsername(committed.username, &found, &err));
    CHECK(found.has_value());
    deleteUser(lease.connection(), committed.id);
}

void testPoolConcurrency(const cofinance::db::MySqlConfig& config) {
    std::printf("[case] 100 线程并发借还与超时\n");
    cofinance::db::ConnectionPool pool;
    std::string err;
    CHECK(pool.init(config, &err));
    if (!static_cast<bool>(pool.capacity())) {
        return;
    }

    std::atomic<int> failures{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(100);
    for (int t = 0; t < 100; ++t) {
        threads.emplace_back([&pool, &failures, &start]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 10; ++i) {
                std::string thread_err;
                auto lease = pool.acquire(5000, &thread_err);
                if (!lease) {
                    failures.fetch_add(1);
                    continue;
                }
                cofinance::db::SqlRow row;
                bool has_row = false;
                if (!queryRow(lease.connection(), "SELECT 1", {}, &row, &has_row, &thread_err) ||
                    !has_row) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures.load() == 0);

    std::vector<cofinance::db::ConnectionPool::Lease> leases;
    for (std::size_t i = 0; i < config.pool_size; ++i) {
        leases.push_back(pool.acquire(1000, &err));
        CHECK(static_cast<bool>(leases.back()));
    }
    auto timeout_lease = pool.acquire(80, &err);
    CHECK(!timeout_lease);
    CHECK(err.find("超时") != std::string::npos);
    leases.clear();
    CHECK(pool.idleCount() == config.pool_size);
}

void testReconnect(const cofinance::db::MySqlConfig& config,
                   cofinance::db::ConnectionPool& main_pool) {
    std::printf("[case] 数据库连接被杀后的重建\n");
    cofinance::db::MySqlConfig one = config;
    one.pool_size = 1;
    cofinance::db::ConnectionPool pool;
    std::string err;
    CHECK(pool.init(one, &err));
    auto victim = pool.acquire(1000, &err);
    auto killer = main_pool.acquire(1000, &err);
    CHECK(static_cast<bool>(victim));
    CHECK(static_cast<bool>(killer));
    if (!victim || !killer) {
        return;
    }

    std::string id;
    CHECK(queryScalar(victim.connection(), "SELECT CONNECTION_ID()", {}, &id, &err));
    CHECK(!id.empty());
    CHECK(killer.connection().execute("KILL " + id, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    victim.release();

    auto recovered = pool.acquire(3000, &err);
    CHECK(static_cast<bool>(recovered));
    if (recovered) {
        std::string one_value;
        CHECK(queryScalar(recovered.connection(), "SELECT 1", {}, &one_value, &err));
        CHECK(one_value == "1");
    }
}

void testShutdownWakesWaiters(const cofinance::db::MySqlConfig& config) {
    std::printf("[case] shutdown 唤醒等待者\n");
    cofinance::db::ConnectionPool pool;
    std::string err;
    CHECK(pool.init(config, &err));
    std::vector<cofinance::db::ConnectionPool::Lease> leases;
    for (std::size_t i = 0; i < config.pool_size; ++i) {
        leases.push_back(pool.acquire(1000, &err));
    }

    std::atomic<bool> got_lease{false};
    std::thread waiter([&pool, &got_lease]() {
        std::string wait_err;
        auto lease = pool.acquire(5000, &wait_err);
        got_lease.store(static_cast<bool>(lease));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pool.shutdown();
    waiter.join();
    CHECK(!got_lease.load());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/server.conf";
    const std::string schema_path = argc > 2 ? argv[2] : "sql/schema.sql";

    cofinance::Config cfg;
    std::string err;
    if (!cfg.loadFile(config_path, &err)) {
        std::fprintf(stderr, "P3 测试配置加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    const std::string secrets_path = siblingPath(config_path, "secrets.conf");
    if (!cfg.loadFile(secrets_path, &err)) {
        std::fprintf(stderr, "P3 测试凭据加载失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    cofinance::db::MySqlConfig config = makeConfig(cfg, 4);
    testConnectionAndSchema(config, schema_path);

    cofinance::db::ConnectionPool pool;
    if (!pool.init(config, &err)) {
        std::fprintf(stderr, "P3 测试连接池初始化失败: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    testPreparedStatementAndInjection(pool);
    testTransactionGuard(pool);
    testReconnect(config, pool);
    testPoolConcurrency(config);
    testShutdownWakesWaiters(config);

    if (g_failed != 0) {
        std::fprintf(stderr, "P3 数据库测试失败: %d/%d\n", g_failed, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("P3 数据库测试通过: %d 个检查点\n", g_checks);
    return EXIT_SUCCESS;
}
