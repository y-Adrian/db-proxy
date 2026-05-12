#include "pool/connection_pool.h"
#include "pool/backend_connection.h"

// 集成测试：环境变量二选一启用（默认跳过，便于无数据库的 CI）。
//   DBPROXY_TEST_MYSQL=1 — 127.0.0.1:3306 root / 空密码 / test
//   DBPROXY_TEST_PG=1    — PGHOST、PGPORT、PGUSER、PGPASSWORD、PGDATABASE（与 test_with_pg.sh 一致）

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dbproxy;

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};

void worker(ConnectionPool& pool) {
    for (int i = 0; i < 10; ++i) {
        auto conn = pool.getConnection(std::chrono::seconds(5));
        if (conn) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pool.returnConnection(conn);
            success_count++;
        } else {
            fail_count++;
        }
    }
}

static std::string getenv_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : fallback;
}

static uint16_t getenv_port(const char* name, uint16_t def) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return def;
    }
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (end == v || n <= 0 || n > 65535) {
        return def;
    }
    return static_cast<uint16_t>(n);
}

static int run_mysql_pool_test() {
    success_count = 0;
    fail_count = 0;
    std::cout << "=== Connection Pool Test (MySQL) ===" << std::endl;

    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 5, 50,
                        std::chrono::seconds(30), std::chrono::seconds(5));

    if (!pool.warmup()) {
        std::cerr << "Failed to warmup pool" << std::endl;
        return 1;
    }

    std::cout << "Pool warmed up: " << pool.totalConnections() << " connections" << std::endl;

    const int num_threads = 20;
    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, std::ref(pool));
    }
    for (auto& t : threads) {
        t.join();
    }
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "\n=== Test Results (MySQL) ===" << std::endl;
    std::cout << "Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "Success: " << success_count.load() << std::endl;
    std::cout << "Failed: " << fail_count.load() << std::endl;
    std::cout << "Total: " << (success_count.load() + fail_count.load()) << std::endl;
    std::cout << "QPS: " << (success_count.load() * 1000.0 / duration.count()) << std::endl;
    std::cout << "Pool stats: total=" << pool.totalConnections() << " idle=" << pool.idleConnections()
              << " busy=" << pool.busyConnections() << std::endl;
    return 0;
}

#if DBPROXY_ENABLE_POSTGRES

static int run_postgres_pool_test() {
    success_count = 0;
    fail_count = 0;
    std::cout << "=== Connection Pool Test (PostgreSQL) ===" << std::endl;

    const std::string host = getenv_or("PGHOST", "127.0.0.1");
    const uint16_t port = getenv_port("PGPORT", 5432);
    const std::string user = getenv_or("PGUSER", getenv_or("USER", "postgres"));
    const std::string password = getenv_or("PGPASSWORD", "");
    const std::string database = getenv_or("PGDATABASE", "test");

    std::cout << "Using " << host << ":" << port << " db=" << database << " user=" << user << std::endl;

    ConnectionPool pool(host, port, user, password, database, 5, 50, std::chrono::seconds(30),
                        std::chrono::seconds(5), BackendProtocol::PostgreSQL);

    if (!pool.warmup()) {
        std::cerr << "Failed to warmup pool" << std::endl;
        return 1;
    }

    std::cout << "Pool warmed up: " << pool.totalConnections() << " connections" << std::endl;

    const int num_threads = 20;
    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, std::ref(pool));
    }
    for (auto& t : threads) {
        t.join();
    }
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "\n=== Test Results (PostgreSQL) ===" << std::endl;
    std::cout << "Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "Success: " << success_count.load() << std::endl;
    std::cout << "Failed: " << fail_count.load() << std::endl;
    std::cout << "Total: " << (success_count.load() + fail_count.load()) << std::endl;
    std::cout << "QPS: " << (success_count.load() * 1000.0 / duration.count()) << std::endl;
    std::cout << "Pool stats: total=" << pool.totalConnections() << " idle=" << pool.idleConnections()
              << " busy=" << pool.busyConnections() << std::endl;
    return 0;
}

#endif  // DBPROXY_ENABLE_POSTGRES

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    const char* pg = std::getenv("DBPROXY_TEST_PG");
    const char* my = std::getenv("DBPROXY_TEST_MYSQL");

    // 与 MySQL 对称：本机有库且显式设置环境变量时才跑，避免 CI/无数据库环境失败。
    if (pg && std::string(pg) == "1") {
#if DBPROXY_ENABLE_POSTGRES
        return run_postgres_pool_test();
#else
        std::cout << "[SKIP] PostgreSQL pool not compiled (need OpenSSL and "
                     "DBPROXY_ENABLE_POSTGRES=ON)\n";
        return 0;
#endif
    }

    if (my && std::string(my) == "1") {
        return run_mysql_pool_test();
    }

    std::cout << "[SKIP] Set DBPROXY_TEST_MYSQL=1 (127.0.0.1:3306 root/''/test) or "
                 "DBPROXY_TEST_PG=1 (PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE) to run.\n";
    return 0;
}
