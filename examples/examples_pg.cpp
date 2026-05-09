/**
 * @file examples_pg.cpp
 * @brief DB-Proxy PostgreSQL 真实后端场景化用例集
 *
 * 本文件通过 db-proxy 的 PostgreSQL 协议实现真正连接 PG 后端，
 * 执行查询并展示结果数据。
 *
 * 前置条件：
 *   - PostgreSQL 服务运行在 127.0.0.1:5432
 *   - 存在可连接的用户和数据库
 *
 * 如 PG 不可用，示例会优雅跳过，不会崩溃。
 */

#include "pool/connection_pool.h"
#include "pool/pool_manager.h"
#include "monitor/metrics.h"
#include "monitor/statistics.h"
#include "core/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdlib>

using namespace dbproxy;

// ============================================================================
// PostgreSQL 连接配置（根据本地环境自动检测）
// ============================================================================

struct PGConfig {
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string user;
    std::string password;
    std::string database = "test";
};

/** 从环境变量或默认值构建 PG 配置 */
PGConfig detectPGConfig() {
    PGConfig cfg;
    // 优先使用环境变量，方便 Docker 场景
    const char* env_host = std::getenv("PGHOST");
    const char* env_port = std::getenv("PGPORT");
    const char* env_user = std::getenv("PGUSER");
    const char* env_pass = std::getenv("PGPASSWORD");
    const char* env_db   = std::getenv("PGDATABASE");

    if (env_host) cfg.host = env_host;
    if (env_port) cfg.port = std::stoi(env_port);
    if (env_user) cfg.user = env_user;
    if (env_pass) cfg.password = env_pass;
    if (env_db)   cfg.database = env_db;

    // macOS Homebrew PostgreSQL 默认使用当前系统用户名，无密码
    if (cfg.user.empty()) {
        cfg.user = std::getenv("USER") ? std::getenv("USER") : "postgres";
    }
    return cfg;
}

/** 创建 PG 连接池，失败返回 nullptr */
std::shared_ptr<ConnectionPool> createPGPool(const PGConfig& cfg,
                                              size_t min_conn = 3,
                                              size_t max_conn = 10) {
    auto pool = std::make_shared<ConnectionPool>(
        cfg.host, cfg.port, cfg.user, cfg.password, cfg.database,
        min_conn, max_conn,
        std::chrono::seconds(30),
        std::chrono::seconds(5),
        BackendProtocol::PostgreSQL);
    return pool;
}

/** 打印查询结果的辅助函数 */
void printResult(const ConnectionPtr& conn, const std::string& sql) {
    if (!conn) return;
    if (conn->execute(sql)) {
        const auto& cols = conn->resultColumns();
        const auto& rows = conn->resultRows();

        // 打印列头
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) std::cout << " | ";
            std::cout << cols[i].name;
        }
        std::cout << "\n";

        // 打印分隔线
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) std::cout << "-+-";
            std::cout << "--------";
        }
        std::cout << "\n";

        // 打印行数据
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) std::cout << " | ";
                std::cout << row[i];
            }
            std::cout << "\n";
        }

        if (rows.empty()) {
            std::cout << "(0 rows)\n";
        }
        if (conn->affectedRows() > 0) {
            std::cout << "Affected rows: " << conn->affectedRows() << "\n";
        }
    } else {
        std::cout << "  [ERROR] " << conn->lastError() << "\n";
    }
}

/** 执行单条 SQL 不打印结果，仅报告成功/失败 */
bool execSQL(const ConnectionPtr& conn, const std::string& sql) {
    if (!conn) return false;
    if (conn->execute(sql)) {
        return true;
    } else {
        std::cout << "  [ERROR] " << sql.substr(0, 50) << " -> " << conn->lastError() << "\n";
        return false;
    }
}

// ============================================================================
// 场景 1: PostgreSQL 基础连接池使用
// ============================================================================

void example_pg_basic_usage(const PGConfig& cfg) {
    std::cout << "\n=== 场景 1: PostgreSQL 基础连接池使用 ===\n";

    auto pool = createPGPool(cfg, 2, 5);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过此场景\n";
        return;
    }
    std::cout << "  ✓ 连接池预热成功: 总=" << pool->totalConnections()
              << ", 空闲=" << pool->idleConnections() << "\n";

    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (!conn) {
        std::cout << "  ✗ 获取连接失败\n";
        return;
    }

    std::cout << "\n  --- SELECT version() ---\n";
    printResult(conn, "SELECT version()");

    std::cout << "\n  --- SELECT current_database() ---\n";
    printResult(conn, "SELECT current_database()");

    std::cout << "\n  --- SELECT current_user ---\n";
    printResult(conn, "SELECT current_user");

    pool->returnConnection(conn);
    std::cout << "\n  ✓ 连接已归还，池状态: 总=" << pool->totalConnections()
              << ", 空闲=" << pool->idleConnections()
              << ", 忙碌=" << pool->busyConnections() << "\n";
}

// ============================================================================
// 场景 2: PostgreSQL 协议特性
// ============================================================================

void example_pg_protocol_features(const PGConfig& cfg) {
    std::cout << "\n=== 场景 2: PostgreSQL 协议特性 ===\n";

    auto pool = createPGPool(cfg, 2, 5);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过\n";
        return;
    }

    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (!conn) return;

    // PostgreSQL 特有查询
    std::cout << "\n  --- JSON 操作 ---\n";
    printResult(conn, "SELECT '{\"key\": \"value\"}'::json ->> 'key' AS json_value");

    std::cout << "\n  --- 数组操作 ---\n";
    printResult(conn, "SELECT ARRAY[1, 2, 3] || ARRAY[4, 5] AS merged_array");

    std::cout << "\n  --- 范围类型 ---\n";
    printResult(conn, "SELECT '[1,10]'::int4range AS num_range");

    std::cout << "\n  --- 时间函数 ---\n";
    printResult(conn, "SELECT now() AS current_time, date_trunc('hour', now()) AS hour_start");

    pool->returnConnection(conn);
}

// ============================================================================
// 场景 3: PostgreSQL 事务处理
// ============================================================================

void example_pg_transaction(const PGConfig& cfg) {
    std::cout << "\n=== 场景 3: PostgreSQL 事务处理 ===\n";

    auto pool = createPGPool(cfg, 1, 3);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过\n";
        return;
    }

    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (!conn) return;

    // 创建测试表
    std::cout << "  创建测试表...\n";
    execSQL(conn, "DROP TABLE IF EXISTS pg_proxy_test");
    execSQL(conn, "CREATE TABLE pg_proxy_test (id SERIAL PRIMARY KEY, name VARCHAR(100))");

    // 事务：提交
    std::cout << "\n  --- 事务提交示例 ---\n";
    execSQL(conn, "BEGIN");
    execSQL(conn, "INSERT INTO pg_proxy_test (name) VALUES ('Alice')");
    execSQL(conn, "INSERT INTO pg_proxy_test (name) VALUES ('Bob')");
    execSQL(conn, "COMMIT");
    std::cout << "  事务已提交\n";

    std::cout << "\n  --- 查询已提交的数据 ---\n";
    printResult(conn, "SELECT * FROM pg_proxy_test");

    // 事务：回滚
    std::cout << "\n  --- 事务回滚示例 ---\n";
    execSQL(conn, "BEGIN");
    execSQL(conn, "INSERT INTO pg_proxy_test (name) VALUES ('Charlie')");
    std::cout << "  已插入 Charlie（未提交）\n";
    execSQL(conn, "ROLLBACK");
    std::cout << "  事务已回滚\n";

    std::cout << "\n  --- Charlie 不应存在 ---\n";
    printResult(conn, "SELECT * FROM pg_proxy_test");

    // 清理
    execSQL(conn, "DROP TABLE pg_proxy_test");
    pool->returnConnection(conn);
}

// ============================================================================
// 场景 4: PostgreSQL LISTEN/NOTIFY
// ============================================================================

void example_pg_listen_notify(const PGConfig& cfg) {
    std::cout << "\n=== 场景 4: PostgreSQL LISTEN/NOTIFY ===\n";

    auto pool = createPGPool(cfg, 1, 2);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过\n";
        return;
    }

    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (!conn) return;

    // LISTEN
    std::cout << "  执行 LISTEN db_proxy_channel...\n";
    execSQL(conn, "LISTEN db_proxy_channel");

    // NOTIFY（同一连接可同时 LISTEN 和 NOTIFY）
    std::cout << "  执行 NOTIFY db_proxy_channel...\n";
    execSQL(conn, "NOTIFY db_proxy_channel, 'hello from db-proxy'");

    std::cout << "  ✓ LISTEN/NOTIFY 命令已发送\n";
    std::cout << "  注: 异步通知需通过协议的 NotificationResponse 消息接收\n";

    pool->returnConnection(conn);
}

// ============================================================================
// 场景 5: PostgreSQL 连接池健康检查
// ============================================================================

void example_pg_health_check(const PGConfig& cfg) {
    std::cout << "\n=== 场景 5: PostgreSQL 连接池健康检查 ===\n";

    auto pool = createPGPool(cfg, 3, 10);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过\n";
        return;
    }

    std::cout << "  健康检查前: 总=" << pool->totalConnections()
              << ", 空闲=" << pool->idleConnections() << "\n";

    pool->healthCheck();

    std::cout << "  健康检查后: 总=" << pool->totalConnections()
              << ", 空闲=" << pool->idleConnections() << "\n";

    // 通过查询展示 PG 内置监控
    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (conn) {
        std::cout << "\n  --- pg_stat_activity（当前连接）---\n";
        printResult(conn, "SELECT pid, usename, application_name, state, query "
                          "FROM pg_stat_activity WHERE datname = current_database() LIMIT 5");
        pool->returnConnection(conn);
    }
}

// ============================================================================
// 场景 6: PostgreSQL 多连接池管理
// ============================================================================

void example_pg_multi_database(const PGConfig& cfg) {
    std::cout << "\n=== 场景 6: PostgreSQL 多连接池管理 ===\n";

    auto& manager = PoolManager::instance();

    // 使用同一 PG 实例的不同数据库
    bool ok = true;
    ok &= manager.addPool("pg_default", cfg.host, cfg.port,
                           cfg.user, cfg.password, cfg.database,
                           2, 10, BackendProtocol::PostgreSQL);
    // 尝试连接 postgres 库（系统默认库，通常存在）
    ok &= manager.addPool("pg_system", cfg.host, cfg.port,
                           cfg.user, cfg.password, "postgres",
                           2, 5, BackendProtocol::PostgreSQL);

    if (!ok) {
        std::cout << "  ⚠ 部分连接池创建失败\n";
    }

    // 展示所有池状态
    auto stats = manager.getAllStats();
    for (const auto& s : stats) {
        std::cout << "  池 [" << s.name << "]: 总=" << s.total
                  << ", 空闲=" << s.idle << ", 忙碌=" << s.busy << "\n";
    }

    // 从池中获取连接并查询
    auto conn = manager.getConnection("pg_default");
    if (conn) {
        std::cout << "\n  --- 从 pg_default 查询 ---\n";
        printResult(conn, "SELECT current_database(), inet_server_port()");
        manager.returnConnection("pg_default", conn);
    }

    auto conn2 = manager.getConnection("pg_system");
    if (conn2) {
        std::cout << "\n  --- 从 pg_system 查询 ---\n";
        printResult(conn2, "SELECT current_database(), inet_server_port()");
        manager.returnConnection("pg_system", conn2);
    }

    manager.shutdownAll();
    std::cout << "\n  ✓ 所有连接池已关闭\n";
}

// ============================================================================
// 场景 7: PostgreSQL 性能监控
// ============================================================================

void example_pg_metrics(const PGConfig& cfg) {
    std::cout << "\n=== 场景 7: PostgreSQL 性能监控 ===\n";

    auto pool = createPGPool(cfg, 2, 5);
    if (!pool->warmup()) {
        std::cout << "  ✗ 连接池预热失败，跳过\n";
        return;
    }

    auto& stats = Statistics::instance();

    // 模拟一批 PG 查询统计
    std::vector<std::pair<std::string, std::string>> pg_queries = {
        {"SELECT * FROM users WHERE id = $1", "SELECT"},
        {"INSERT INTO orders VALUES ($1, $2, $3)", "INSERT"},
        {"SELECT date_trunc('hour', created_at) FROM events", "SELECT"},
        {"UPDATE inventory SET stock = stock - $1 WHERE id = $2", "UPDATE"},
        {"SELECT pg_database_size(current_database())", "SELECT"}
    };

    for (const auto& [sql, type] : pg_queries) {
        stats.recordQuery(sql, type, cfg.database, 10 + (rand() % 50));
    }

    std::cout << "  QPS: " << stats.getQPS() << "\n";
    std::cout << "  Read QPS: " << stats.getReadQPS() << "\n";
    std::cout << "  Write QPS: " << stats.getWriteQPS() << "\n";

    // 通过真实 PG 查询获取数据库统计
    auto conn = pool->getConnection(std::chrono::seconds(5));
    if (conn) {
        std::cout << "\n  --- pg_stat_database ---\n";
        printResult(conn, "SELECT datname, numbackends, xact_commit, xact_rollback, blks_read "
                          "FROM pg_stat_database WHERE datname = current_database()");
        pool->returnConnection(conn);
    }
}

// ============================================================================
// 主函数
// ============================================================================

int main() {
    Logger::instance().init("./logs/pg_examples.log", LogLevel::INFO);

    std::cout << "========================================\n";
    std::cout << "   DB-Proxy PostgreSQL 场景化用例集\n";
    std::cout << "   （真实 PG 后端协议连接）\n";
    std::cout << "========================================\n";

    PGConfig cfg = detectPGConfig();
    std::cout << "\nPG 配置: " << cfg.user << "@" << cfg.host
              << ":" << cfg.port << "/" << cfg.database << "\n";

    // 先测试 PG 是否可用
    {
        auto test_pool = createPGPool(cfg, 1, 1);
        if (!test_pool->warmup()) {
            std::cout << "\n✗ PostgreSQL 不可用，请确认服务已启动。\n";
            std::cout << "  启动方式:\n";
            std::cout << "    Docker: docker compose -f docker-compose-pg.yml up -d\n";
            std::cout << "    Homebrew: brew services start postgresql@18\n";
            std::cout << "  环境变量: PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE\n";
            return 1;
        }
        auto conn = test_pool->getConnection(std::chrono::seconds(3));
        if (conn) {
            std::cout << "✓ PostgreSQL 连接成功\n";
            test_pool->returnConnection(conn);
        }
    }

    // 运行所有场景
    example_pg_basic_usage(cfg);
    example_pg_protocol_features(cfg);
    example_pg_transaction(cfg);
    example_pg_listen_notify(cfg);
    example_pg_health_check(cfg);
    example_pg_multi_database(cfg);
    example_pg_metrics(cfg);

    std::cout << "\n========================================\n";
    std::cout << "   用例测试完成\n";
    std::cout << "========================================\n";

    return 0;
}
