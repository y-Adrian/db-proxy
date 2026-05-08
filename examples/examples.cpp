/**
 * @file examples.cpp
 * @brief DB-Proxy 场景化用例集
 * 
 * 本文件展示了 db-proxy 的各种使用场景和示例代码
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
// 场景 1: 基础连接池使用
// ============================================================================

void example_basic_usage() {
    std::cout << "\n=== 场景 1: 基础连接池使用 ===" << std::endl;
    
    // 创建连接池
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test",
                      5,   // 最小连接数
                      20,  // 最大连接数
                      std::chrono::seconds(30),  // 最大空闲时间
                      std::chrono::seconds(5));   // 获取连接超时
    
    // 预热连接池
    if (pool.warmup()) {
        std::cout << "连接池预热成功" << std::endl;
    }
    
    // 获取连接
    auto conn = pool.getConnection(std::chrono::seconds(5));
    if (conn) {
        std::cout << "获取连接成功, ID: " << conn->id() << std::endl;
        
        // 使用连接执行查询
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
// 场景 2: 多线程并发使用
// ============================================================================

void example_concurrent_usage() {
    std::cout << "\n=== 场景 2: 多线程并发使用 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 10, 50,
                        std::chrono::seconds(30), std::chrono::seconds(10));
    pool.warmup();
    
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    const int num_threads = 10;
    const int queries_per_thread = 20;
    
    std::vector<std::thread> threads;
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&pool, &success_count, &fail_count, queries_per_thread, i]() {
            for (int j = 0; j < queries_per_thread; ++j) {
                auto conn = pool.getConnection(std::chrono::seconds(5));
                if (conn) {
                    // 模拟查询延迟
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    pool.returnConnection(conn);
                    success_count++;
                } else {
                    fail_count++;
                }
            }
            std::cout << "线程 " << i << " 完成" << std::endl;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n--- 测试结果 ---" << std::endl;
    std::cout << "总耗时: " << duration.count() << " ms" << std::endl;
    std::cout << "成功: " << success_count.load() << std::endl;
    std::cout << "失败: " << fail_count.load() << std::endl;
    std::cout << "QPS: " << (success_count.load() * 1000.0 / duration.count()) << std::endl;
}

// ============================================================================
// 场景 3: 连接池动态调整
// ============================================================================

void example_pool_adjustment() {
    std::cout << "\n=== 场景 3: 连接池动态调整 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 5, 30,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    pool.warmup();
    
    std::cout << "初始状态: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << std::endl;
    
    // 模拟高负载
    std::vector<std::shared_ptr<Connection>> held_conns;
    for (int i = 0; i < 20; ++i) {
        auto conn = pool.getConnection(std::chrono::seconds(1));
        if (conn) {
            held_conns.push_back(conn);
        }
    }
    
    std::cout << "高负载后: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << std::endl;
    
    // 释放连接
    for (auto& conn : held_conns) {
        pool.returnConnection(conn);
    }
    held_conns.clear();
    
    std::cout << "释放后: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << std::endl;
    
    // 模拟空闲 - 等待清理
    std::this_thread::sleep_for(std::chrono::seconds(2));
    pool.cleanIdleConnections();
    
    std::cout << "清理后: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << std::endl;
}

// ============================================================================
// 场景 4: 健康检查
// ============================================================================

void example_health_check() {
    std::cout << "\n=== 场景 4: 健康检查 ===" << std::endl;
    
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 5, 10,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    pool.warmup();
    
    std::cout << "健康检查前: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections() << std::endl;
    
    // 执行健康检查
    pool.healthCheck();
    
    std::cout << "健康检查后: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections() << std::endl;
    
    // 启动定时健康检查线程
    std::atomic<bool> running{true};
    std::thread health_check([&pool, &running]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            pool.healthCheck();
            std::cout << "[健康检查] 总=" << pool.totalConnections()
                      << ", 空闲=" << pool.idleConnections()
                      << ", 忙碌=" << pool.busyConnections() << std::endl;
        }
    });
    
    // 模拟一些请求
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    running.store(false);
    health_check.join();
}

// ============================================================================
// 场景 5: 多数据库连接池管理
// ============================================================================

void example_multi_database() {
    std::cout << "\n=== 场景 5: 多数据库连接池管理 ===" << std::endl;
    
    auto& manager = PoolManager::instance();
    
    // 添加多个数据库连接池
    manager.addPool("db1", "127.0.0.1", 3306, "root", "", "test", 5, 20);
    manager.addPool("db2", "127.0.0.1", 3306, "root", "", "test2", 3, 10);
    manager.addPool("readonly", "127.0.0.1", 3306, "root", "", "test", 5, 30);
    
    std::cout << "已添加 3 个连接池" << std::endl;
    
    // 获取所有池状态
    auto stats = manager.getAllStats();
    for (const auto& s : stats) {
        std::cout << "池 [" << s.name << "]: 总=" << s.total
                  << ", 空闲=" << s.idle
                  << ", 忙碌=" << s.busy << std::endl;
    }
    
    // 从指定池获取连接
    auto conn = manager.getConnection("db1");
    if (conn) {
        std::cout << "从 db1 获取连接成功" << std::endl;
        manager.returnConnection("db1", conn);
    }
    
    // 关闭所有池
    manager.shutdownAll();
    std::cout << "所有连接池已关闭" << std::endl;
}

// ============================================================================
// 场景 6: 性能监控
// ============================================================================

void example_metrics() {
    std::cout << "\n=== 场景 6: 性能监控 ===" << std::endl;
    
    auto& metrics = Metrics::instance();
    auto& stats = Statistics::instance();
    
    // 记录一些查询
    for (int i = 0; i < 100; ++i) {
        metrics.incrementCounter("queries_total");
        
        // 随机延迟
        int latency_ms = 10 + (i % 50);
        metrics.recordLatency("query_latency", std::chrono::milliseconds(latency_ms));
        
        // 记录 SQL 统计
        std::string sql = (i % 3 == 0) ? "SELECT * FROM users" : "INSERT INTO logs VALUES (1)";
        std::string type = (i % 3 == 0) ? "SELECT" : "INSERT";
        stats.recordQuery(sql, type, "test", latency_ms);
    }
    
    // 设置 gauge
    metrics.setGauge("active_connections", 50);
    metrics.setGauge("pool_usage_percent", 75.5);
    
    // 输出 Prometheus 格式
    std::cout << "\n--- Prometheus 格式指标 ---" << std::endl;
    std::cout << metrics.toPrometheusFormat();
    
    // 输出 JSON 统计
    std::cout << "\n--- JSON 格式统计 ---" << std::endl;
    std::cout << stats.toJSON() << std::endl;
    
    // 获取慢查询
    std::cout << "\n--- Top 5 慢查询 ---" << std::endl;
    auto slow_queries = stats.getSlowQueries(5);
    for (const auto& q : slow_queries) {
        std::cout << q.type << " on " << q.database
                  << " (avg: " << q.avg_latency_ms << "ms)" << std::endl;
    }
}

// ============================================================================
// 场景 7: 延迟百分位数计算
// ============================================================================

void example_percentile() {
    std::cout << "\n=== 场景 7: 延迟百分位数计算 ===" << std::endl;
    
    auto& metrics = Metrics::instance();
    
    // 模拟延迟分布: 大部分请求延迟低，少部分很高
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 100);
    
    for (int i = 0; i < 1000; ++i) {
        int latency_ms = dis(gen);
        
        // 添加一些极端值
        if (i % 100 == 0) latency_ms = 500 + dis(gen);
        
        metrics.recordLatency("response_time", std::chrono::milliseconds(latency_ms));
    }
    
    // 获取直方图数据
    auto hist = metrics.getHistogram("response_time");
    
    std::cout << "样本数: " << hist.count << std::endl;
    std::cout << "平均值: " << (hist.sum / hist.count) << " ms" << std::endl;
    std::cout << "最小值: " << hist.min << " ms" << std::endl;
    std::cout << "最大值: " << hist.max << " ms" << std::endl;
    std::cout << "P50: " << hist.p50 << " ms" << std::endl;
    std::cout << "P90: " << hist.p90 << " ms" << std::endl;
    std::cout << "P99: " << hist.p99 << " ms" << std::endl;
    std::cout << "P999: " << hist.p999 << " ms" << std::endl;
}

// ============================================================================
// 场景 8: QPS 计算
// ============================================================================

void example_qps_calculation() {
    std::cout << "\n=== 场景 8: QPS 计算 ===" << std::endl;
    
    auto& stats = Statistics::instance();
    
    // 模拟流量
    for (int sec = 0; sec < 10; ++sec) {
        // 每秒 100-200 个查询
        int qps = 100 + (sec * 10);
        
        for (int i = 0; i < qps; ++i) {
            std::string type = (i % 5 == 0) ? "SELECT" : "INSERT";
            stats.recordQuery("test query", type, "test", 10);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 加速模拟
        
        // 打印当前 QPS
        std::cout << "第 " << sec << " 秒: QPS=" << stats.getQPS()
                  << ", RPS=" << stats.getReadQPS()
                  << ", WPS=" << stats.getWriteQPS() << std::endl;
    }
}

// ============================================================================
// 场景 9: 读写分离路由
// ============================================================================

void example_read_write_routing() {
    std::cout << "\n=== 场景 9: 读写分离路由 ===" << std::endl;
    
    auto& manager = PoolManager::instance();
    
    // 添加主库（写）和从库（读）
    manager.addPool("master", "127.0.0.1", 3306, "root", "", "test", 5, 20);
    manager.addPool("slave1", "127.0.0.1", 3306, "root", "", "test", 5, 20);
    manager.addPool("slave2", "127.0.0.1", 3306, "root", "", "test", 5, 20);
    
    // SQL 类型判断
    auto parseSQL = [](const std::string& sql) -> std::string {
        std::string upper = sql;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        
        if (upper.find("SELECT") == 0 || upper.find("SHOW") == 0) {
            return "read";
        }
        return "write";
    };
    
    // 路由函数
    auto route = [&](const std::string& sql) {
        std::string type = parseSQL(sql);
        if (type == "read") {
            // 简单轮询选择从库
            static int slave_idx = 0;
            std::string pool_name = "slave" + std::to_string(++slave_idx % 2 + 1);
            std::cout << "SQL: " << sql << " -> 路由到 [" << pool_name << "]" << std::endl;
        } else {
            std::cout << "SQL: " << sql << " -> 路由到 [master]" << std::endl;
        }
    };
    
    // 测试路由
    route("SELECT * FROM users WHERE id = 1");
    route("SELECT * FROM orders");
    route("INSERT INTO users VALUES (1, 'test')");
    route("UPDATE users SET name = 'new' WHERE id = 1");
    route("SELECT COUNT(*) FROM logs");
    route("DELETE FROM logs WHERE created_at < '2024-01-01'");
    
    manager.shutdownAll();
}

// ============================================================================
// 场景 10: 熔断器实现
// ============================================================================

class CircuitBreaker {
public:
    enum class State { CLOSED, OPEN, HALF_OPEN };
    
    CircuitBreaker(int failure_threshold = 5, 
                   std::chrono::seconds reset_timeout = std::chrono::seconds(30))
        : failure_threshold_(failure_threshold), reset_timeout_(reset_timeout) {}
    
    bool allowRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        switch (state_) {
            case State::CLOSED:
                return true;
                
            case State::OPEN:
                if (std::chrono::steady_clock::now() - open_time_ > reset_timeout_) {
                    state_ = State::HALF_OPEN;
                    return true;
                }
                return false;
                
            case State::HALF_OPEN:
                return true;  // 允许一个请求测试
        }
        return true;
    }
    
    void recordSuccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (state_ == State::HALF_OPEN) {
            state_ = State::CLOSED;
            failure_count_ = 0;
        }
    }
    
    void recordFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        failure_count_++;
        
        if (state_ == State::HALF_OPEN) {
            // 测试失败，重新打开
            state_ = State::OPEN;
            open_time_ = std::chrono::steady_clock::now();
        } else if (failure_count_ >= failure_threshold_) {
            state_ = State::OPEN;
            open_time_ = std::chrono::steady_clock::now();
        }
    }
    
    State getState() const { return state_; }
    
private:
    State state_{State::CLOSED};
    int failure_count_{0};
    int failure_threshold_;
    std::chrono::seconds reset_timeout_;
    std::chrono::steady_clock::time_point open_time_;
    std::mutex mutex_;
};

void example_circuit_breaker() {
    std::cout << "\n=== 场景 10: 熔断器 ===" << std::endl;
    
    CircuitBreaker cb(3, std::chrono::seconds(5));
    
    // 模拟失败
    std::cout << "初始状态: ";
    std::cout << (cb.getState() == CircuitBreaker::State::CLOSED ? "CLOSED" : 
                  cb.getState() == CircuitBreaker::State::OPEN ? "OPEN" : "HALF_OPEN") << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        if (cb.allowRequest()) {
            std::cout << "请求 " << i + 1 << ": 允许" << std::endl;
            
            // 模拟失败
            if (i < 4) {
                cb.recordFailure();
                std::cout << "  -> 记录失败" << std::endl;
            } else {
                cb.recordSuccess();
                std::cout << "  -> 记录成功" << std::endl;
            }
        } else {
            std::cout << "请求 " << i + 1 << ": 拒绝 (熔断)" << std::endl;
        }
    }
    
    std::cout << "最终状态: ";
    std::cout << (cb.getState() == CircuitBreaker::State::CLOSED ? "CLOSED" : 
                  cb.getState() == CircuitBreaker::State::OPEN ? "OPEN" : "HALF_OPEN") << std::endl;
}

// ============================================================================
// 主函数
// ============================================================================

int main() {
    Logger::instance().init("", LogLevel::WARN);
    
    std::cout << "========================================" << std::endl;
    std::cout << "       DB-Proxy 场景化用例集" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 注意: 这些示例需要 MySQL 服务器运行才能正常工作
    // 以下用例可以在没有数据库的情况下测试监控部分
    
    // example_basic_usage();           // 需要 MySQL
    // example_concurrent_usage();      // 需要 MySQL
    // example_pool_adjustment();       // 需要 MySQL
    // example_health_check();          // 需要 MySQL
    // example_multi_database();        // 需要 MySQL
    // example_read_write_routing();    // 需要 MySQL
    
    // 以下用例不需要数据库连接
    example_metrics();
    example_percentile();
    example_qps_calculation();
    example_circuit_breaker();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "              用例测试完成" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
