/**
 * @file examples_pg.cpp
 * @brief DB-Proxy PostgreSQL 版本场景化用例集
 * 
 * 本文件展示了 db-proxy 支持 PostgreSQL 协议的使用场景和示例代码
 * 适用于学习、测试和面试演示
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
#include <random>

using namespace dbproxy;

// ============================================================================
// PostgreSQL 配置常量
// ============================================================================

// PostgreSQL 默认端口
constexpr int PG_PORT = 5432;

// PostgreSQL 连接参数结构
struct PGConfig {
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string database;
};

// ============================================================================
// 场景 1: PostgreSQL 基础连接池使用
// ============================================================================

void example_pg_basic_usage() {
    std::cout << "\n=== 场景 1: PostgreSQL 基础连接池使用 ===" << std::endl;
    
    // PostgreSQL 连接配置
    PGConfig config{
        .host = "127.0.0.1",
        .port = PG_PORT,
        .user = "postgres",
        .password = "postgres",
        .database = "test"
    };
    
    // 创建连接池 (PostgreSQL)
    ConnectionPool pool(config.host, config.port, config.user, config.password, config.database,
                        5,   // 最小连接数
                        20,  // 最大连接数
                        std::chrono::seconds(30),  // 最大空闲时间
                        std::chrono::seconds(5));   // 获取连接超时
    
    // 预热连接池
    if (pool.warmup()) {
        std::cout << "PostgreSQL 连接池预热成功" << std::endl;
    }
    
    // 获取连接
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (conn) {
        std::cout << "获取连接成功, ID: " << conn->id() << std::endl;
        
        // 使用连接执行查询 (PostgreSQL 语法)
        if (conn->execute("SELECT 1")) {
            std::cout << "查询执行成功" << std::endl;
        }
        
        // 归还连接
        pool.returnConnection(conn);
        std::cout << "连接已归还" << std::endl;
    }
    
    // 打印状态
    std::cout << "池状态: 总连接=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << std::endl;
}

// ============================================================================
// 场景 2: PostgreSQL 协议特性使用
// ============================================================================

void example_pg_protocol_features() {
    std::cout << "\n=== 场景 2: PostgreSQL 协议特性 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", PG_PORT, "postgres", "postgres", "test", 
                        5, 20, std::chrono::seconds(30), std::chrono::seconds(5));
    
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (!conn) {
        std::cout << "无法获取连接" << std::endl;
        return;
    }
    
    // PostgreSQL 特有查询
    std::vector<std::string> pg_queries = {
        // 版本查询
        "SELECT version()",
        // 当前数据库
        "SELECT current_database()",
        // 当前用户
        "SELECT current_user",
        // 当前时间
        "SELECT now()",
        // 序列操作
        "CREATE SEQUENCE IF NOT EXISTS test_seq",
        "SELECT nextval('test_seq')",
        // JSON 操作 (PostgreSQL 特有)
        "SELECT '{\"key\": \"value\"}'::json ->> 'key'",
        // 数组操作 (PostgreSQL 特有)
        "SELECT ARRAY[1, 2, 3] || ARRAY[4, 5]",
        // 范围类型 (PostgreSQL 特有)
        "SELECT '[1,10]'::int4range"
    };
    
    for (const auto& sql : pg_queries) {
        if (conn->execute(sql)) {
            std::cout << "OK: " << sql.substr(0, 40) << "..." << std::endl;
        } else {
            std::cout << "FAIL: " << sql << std::endl;
        }
    }
    
    pool.returnConnection(conn);
}

// ============================================================================
// 场景 3: PostgreSQL 事务处理
// ============================================================================

void example_pg_transaction() {
    std::cout << "\n=== 场景 3: PostgreSQL 事务处理 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", PG_PORT, "postgres", "postgres", "test",
                        5, 20, std::chrono::seconds(30), std::chrono::seconds(5));
    
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (!conn) {
        std::cout << "无法获取连接" << std::endl;
        return;
    }
    
    // PostgreSQL 事务控制
    std::cout << "开始事务..." << std::endl;
    
    // 开始事务
    conn->execute("BEGIN");
    
    // 创建测试表
    conn->execute("CREATE TABLE IF NOT EXISTS pg_test (id SERIAL PRIMARY KEY, name VARCHAR(100))");
    
    // 插入数据
    conn->execute("INSERT INTO pg_test (name) VALUES ('Alice')");
    conn->execute("INSERT INTO pg_test (name) VALUES ('Bob')");
    
    // 查询
    if (conn->execute("SELECT * FROM pg_test")) {
        std::cout << "查询成功" << std::endl;
    }
    
    // 提交事务
    conn->execute("COMMIT");
    std::cout << "事务已提交" << std::endl;
    
    // 回滚示例 (演示用，不实际执行)
    std::cout << "\n回滚示例:" << std::endl;
    conn->execute("BEGIN");
    conn->execute("INSERT INTO pg_test (name) VALUES ('Charlie')");
    conn->execute("ROLLBACK");
    std::cout << "事务已回滚" << std::endl;
    
    pool.returnConnection(conn);
}

// ============================================================================
// 场景 4: PostgreSQL 连接参数
// ============================================================================

void example_pg_connection_params() {
    std::cout << "\n=== 场景 4: PostgreSQL 连接参数 ===" << std::endl;
    
    // PostgreSQL 支持丰富的连接参数
    struct PGConnectionParams {
        std::string application_name = "db-proxy-demo";
        std::string client_encoding = "UTF8";
        std::string timezone = "Asia/Shanghai";
        int connect_timeout = 10;
        int statement_timeout = 30000;  // 30秒
        bool binary_output = false;
    };
    
    PGConnectionParams params;
    
    std::cout << "PostgreSQL 连接参数配置:" << std::endl;
    std::cout << "  application_name: " << params.application_name << std::endl;
    std::cout << "  client_encoding: " << params.client_encoding << std::endl;
    std::cout << "  timezone: " << params.timezone << std::endl;
    std::cout << "  connect_timeout: " << params.connect_timeout << "s" << std::endl;
    std::cout << "  statement_timeout: " << params.statement_timeout << "ms" << std::endl;
    std::cout << "  binary_output: " << (params.binary_output ? "true" : "false") << std::endl;
    
    // 应用场景
    std::cout << "\n应用场景:" << std::endl;
    std::cout << "  - application_name: 用于 pg_stat_activity 查看连接来源" << std::endl;
    std::cout << "  - statement_timeout: 防止慢查询占用连接" << std::endl;
    std::cout << "  - binary_output: 提高大量数据传输效率" << std::endl;
}

// ============================================================================
// 场景 5: PostgreSQL LISTEN/NOTIFY (发布订阅)
// ============================================================================

void example_pg_listen_notify() {
    std::cout << "\n=== 场景 5: PostgreSQL LISTEN/NOTIFY ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", PG_PORT, "postgres", "postgres", "test",
                        2, 10, std::chrono::seconds(30), std::chrono::seconds(5));
    
    // 注意: LISTEN/NOTIFY 是异步机制，需要长连接
    // 这里演示概念，实际使用需要单独的连接处理通知
    
    std::cout << "PostgreSQL LISTEN/NOTIFY 机制:" << std::endl;
    std::cout << "  1. NOTIFY 'channel': 发送通知" << std::endl;
    std::cout << "  2. LISTEN 'channel': 订阅通道" << std::endl;
    std::cout << "  3. 通知通过连接异步传递" << std::endl;
    
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (!conn) {
        std::cout << "无法获取连接" << std::endl;
        return;
    }
    
    // 订阅通知
    conn->execute("LISTEN my_channel");
    std::cout << "已订阅 my_channel" << std::endl;
    
    // 发送通知 (通常在另一个连接/进程中)
    conn->execute("NOTIFY my_channel, 'Hello from PostgreSQL!'");
    std::cout << "已发送通知" << std::endl;
    
    pool.returnConnection(conn);
}

// ============================================================================
// 场景 6: PostgreSQL COPY 协议 (批量导入)
// ============================================================================

void example_pg_copy() {
    std::cout << "\n=== 场景 6: PostgreSQL COPY 协议 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", PG_PORT, "postgres", "postgres", "test",
                        3, 10, std::chrono::seconds(30), std::chrono::seconds(5));
    
    std::cout << "PostgreSQL COPY 协议特性:" << std::endl;
    std::cout << "  - 高速批量数据导入/导出" << std::endl;
    std::cout << "  - 直接通过协议传输，跳过 SQL 解析" << std::endl;
    std::cout << "  - 比 INSERT 快 3-5 倍" << std::endl;
    
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (!conn) {
        std::cout << "无法获取连接" << std::endl;
        return;
    }
    
    // COPY FROM stdin 示例 (伪代码)
    std::cout << "\nCOPY 使用示例:" << std::endl;
    std::cout << "  COPY my_table FROM stdin;" << std::endl;
    std::cout << "  1\\tAlice\\talice@example.com" << std::endl;
    std::cout << "  2\\tBob\\tbob@example.com" << std::endl;
    std::cout << "  \\." << std::endl;
    
    // COPY TO stdout 示例
    conn->execute("COPY (SELECT 1, 'test') TO stdout");
    
    pool.returnConnection(conn);
}

// ============================================================================
// 场景 7: PostgreSQL 连接池健康检查
// ============================================================================

void example_pg_health_check() {
    std::cout << "\n=== 场景 7: PostgreSQL 健康检查 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", PG_PORT, "postgres", "postgres", "test",
                        5, 10, std::chrono::seconds(30), std::chrono::seconds(5));
    pool.warmup();
    
    std::cout << "健康检查前: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections() << std::endl;
    
    // PostgreSQL 健康检查使用 SELECT 1 (与 MySQL 相同)
    pool.healthCheck();
    
    std::cout << "健康检查后: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections() << std::endl;
    
    // PostgreSQL 特有健康检查
    std::cout << "\nPostgreSQL 特有检查:" << std::endl;
    std::cout << "  - pg_stat_activity: 检查连接状态" << std::endl;
    std::cout << "  - pg_stat_database: 检查数据库健康" << std::endl;
    std::cout << "  - pg_replication_slots: 检查复制槽" << std::endl;
}

// ============================================================================
// 场景 8: MySQL vs PostgreSQL 对比
// ============================================================================

void example_mysql_vs_pg() {
    std::cout << "\n=== 场景 8: MySQL vs PostgreSQL 协议对比 ===" << std::endl;
    
    std::cout << "┌─────────────────┬──────────────────┬──────────────────┐" << std::endl;
    std::cout << "│     特性        │      MySQL       │    PostgreSQL    │" << std::endl;
    std::cout << "├─────────────────┼──────────────────┼──────────────────┤" << std::endl;
    std::cout << "│ 默认端口         │      3306        │      5432        │" << std::endl;
    std::cout << "│ 认证协议        │    SHA256        │    md5/SCRAM     │" << std::endl;
    std::cout << "│ 整数编码        │    LENENC        │      2/4/8B      │" << std::endl;
    std::cout << "│ 字符串编码      │     字节码       │      UTF-8       │" << std::endl;
    std::cout << "│ 预处理语句      │     占位符?      │    占位符$1..   │" << std::endl;
    std::cout << "│ 事务隔离        │    4 级          │    4 级 + SSI   │" << std::endl;
    std::cout << "│ 特殊功能        │   LOAD DATA      │   COPY/LISTEN    │" << std::endl;
    std::cout << "│ 数据类型        │   较简单         │   丰富(范围/JSON)│" << std::endl;
    std::cout << "│ 复制协议        │    binlog        │    WAL          │" << std::endl;
    std::cout << "└─────────────────┴──────────────────┴──────────────────┘" << std::endl;
    
    std::cout << "\n选择建议:" << std::endl;
    std::cout << "  MySQL: 互联网业务，读多写少，简单高效" << std::endl;
    std::cout << "  PostgreSQL: 企业级应用，复杂查询，数据一致性" << std::endl;
}

// ============================================================================
// 场景 9: PostgreSQL 连接池多数据库管理
// ============================================================================

void example_pg_multi_database() {
    std::cout << "\n=== 场景 9: PostgreSQL 多数据库管理 ===" << std::endl;
    
    auto& manager = PoolManager::instance();
    
    // 添加多个 PostgreSQL 数据库连接池
    manager.addPool("pg_main", "127.0.0.1", PG_PORT, "postgres", "postgres", "main_db", 5, 20);
    manager.addPool("pg_analytics", "127.0.0.1", PG_PORT, "postgres", "postgres", "analytics", 3, 10);
    manager.addPool("pg_readonly", "127.0.0.1", PG_PORT, "postgres", "postgres", "main_db", 5, 30);
    
    std::cout << "已添加 3 个 PostgreSQL 连接池" << std::endl;
    
    // 获取所有池状态
    auto stats = manager.getAllStats();
    for (const auto& s : stats) {
        std::cout << "池 [" << s.name << "]: 总=" << s.total
                  << ", 空闲=" << s.idle
                  << ", 忙碌=" << s.busy << std::endl;
    }
    
    // 从指定池获取连接
    auto conn = manager.getConnection("pg_main");
    if (conn) {
        std::cout << "从 pg_main 获取连接成功" << std::endl;
        conn->execute("SELECT current_database()");
        manager.returnConnection("pg_main", conn);
    }
    
    // 关闭所有池
    manager.shutdownAll();
    std::cout << "所有连接池已关闭" << std::endl;
}

// ============================================================================
// 场景 10: PostgreSQL 性能监控
// ============================================================================

void example_pg_metrics() {
    std::cout << "\n=== 场景 10: PostgreSQL 性能监控 ===" << std::endl;
    
    auto& metrics = Metrics::instance();
    auto& stats = Statistics::instance();
    
    // PostgreSQL 特有指标
    std::cout << "PostgreSQL 特有监控指标:" << std::endl;
    std::cout << "  - pg_stat_bgwriter: 后台写入统计" << std::endl;
    std::cout << "  - pg_stat_database: 数据库统计" << std::endl;
    std::cout << "  - pg_stat_user_tables: 表级统计" << std::endl;
    std::cout << "  - pg_stat_activity: 活动连接" << std::endl;
    
    // 模拟查询统计
    std::vector<std::pair<std::string, std::string>> pg_queries = {
        {"SELECT * FROM users WHERE id = $1", "SELECT"},
        {"INSERT INTO orders VALUES ($1, $2, $3)", "INSERT"},
        {"SELECT date_trunc('hour', created_at) FROM events", "SELECT"},
        {"UPDATE inventory SET stock = stock - $1 WHERE id = $2", "UPDATE"},
        {"SELECT pg_database_size(current_database())", "SELECT"}
    };
    
    for (const auto& [sql, type] : pg_queries) {
        stats.recordQuery(sql, type, "postgres", 10 + (rand() % 50));
    }
    
    // 输出统计
    std::cout << "\n--- 统计输出 ---" << std::endl;
    std::cout << "QPS: " << stats.getQPS() << std::endl;
    std::cout << "Read QPS: " << stats.getReadQPS() << std::endl;
    std::cout << "Write QPS: " << stats.getWriteQPS() << std::endl;
    
    // PostgreSQL 格式输出
    std::cout << "\n--- PostgreSQL JSON 输出 ---" << std::endl;
    std::cout << stats.toJSON() << std::endl;
}

// ============================================================================
// 主函数
// ============================================================================

int main() {
    Logger::instance().init("", LogLevel::WARN);
    
    std::cout << "========================================" << std::endl;
    std::cout << "   DB-Proxy PostgreSQL 场景化用例集" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 注意: 这些示例需要 PostgreSQL 服务器运行才能正常工作
    // 以下用例可以在没有数据库的情况下测试监控部分
    
    // example_pg_basic_usage();           // 需要 PostgreSQL
    // example_pg_protocol_features();      // 需要 PostgreSQL
    // example_pg_transaction();            // 需要 PostgreSQL
    // example_pg_listen_notify();          // 需要 PostgreSQL
    // example_pg_copy();                  // 需要 PostgreSQL
    // example_pg_health_check();           // 需要 PostgreSQL
    // example_pg_multi_database();        // 需要 PostgreSQL
    
    // 不需要数据库连接的用例
    example_pg_connection_params();     // 仅配置展示
    example_mysql_vs_pg();               // 对比展示
    example_pg_metrics();                // 监控统计
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "            用例测试完成" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
