/**
 * @file stress_test.cpp
 * @brief 连接池压力测试
 * 
 * 测试场景:
 * 1. 基准测试 - 单线程顺序获取
 * 2. 并发测试 - 多线程同时获取
 * 3. 极限测试 - 超过连接池上限
 * 4. 性能测试 - 连接获取延迟分布
 */

#include "pool/connection_pool.h"
#include "core/logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <cmath>

using namespace dbproxy;

// ============================================================================
// 测试配置
// ============================================================================

struct TestConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password = "";
    std::string database = "test";
    size_t min_connections = 10;
    size_t max_connections = 100;
    int test_duration_seconds = 10;
    int concurrent_threads = 50;
};

TestConfig config;

// ============================================================================
// 统计工具
// ============================================================================

struct LatencyStats {
    double min_ms = 0;
    double max_ms = 0;
    double avg_ms = 0;
    double p50_ms = 0;
    double p90_ms = 0;
    double p99_ms = 0;
    double p999_ms = 0;
};

LatencyStats calculateStats(std::vector<double>& samples) {
    LatencyStats stats;
    
    if (samples.empty()) return stats;
    
    std::sort(samples.begin(), samples.end());
    
    stats.min_ms = samples.front();
    stats.max_ms = samples.back();
    stats.avg_ms = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    
    size_t n = samples.size();
    stats.p50_ms = samples[n * 50 / 100];
    stats.p90_ms = samples[n * 90 / 100];
    stats.p99_ms = samples[n * 99 / 100];
    stats.p999_ms = samples[n * 999 / 1000];
    
    return stats;
}

void printStats(const LatencyStats& stats, const std::string& label) {
    std::cout << "\n--- " << label << " ---\n";
    std::cout << "Min:   " << stats.min_ms << " ms\n";
    std::cout << "Avg:   " << stats.avg_ms << " ms\n";
    std::cout << "P50:   " << stats.p50_ms << " ms\n";
    std::cout << "P90:   " << stats.p90_ms << " ms\n";
    std::cout << "P99:   " << stats.p99_ms << " ms\n";
    std::cout << "P999:  " << stats.p999_ms << " ms\n";
    std::cout << "Max:   " << stats.max_ms << " ms\n";
}

// ============================================================================
// 测试 1: 基准测试 - 单线程顺序获取
// ============================================================================

void test_sequential_gets() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试 1: 单线程顺序获取 (10000 次)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    ConnectionPool pool(config.host, config.port, config.user, 
                        config.password, config.database,
                        config.min_connections, config.max_connections,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    
    if (!pool.warmup()) {
        std::cerr << "连接池预热失败，跳过测试" << std::endl;
        return;
    }
    
    const int total = 10000;
    std::vector<double> latencies;
    latencies.reserve(total);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < total; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto conn = pool.getConnection(std::chrono::seconds(5));
        auto t1 = std::chrono::high_resolution_clock::now();
        
        if (conn) {
            double latency = std::chrono::duration<double, std::milli>(t1 - t0).count();
            latencies.push_back(latency);
            pool.returnConnection(conn);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "总耗时: " << duration.count() << " ms\n";
    std::cout << "吞吐量: " << (total * 1000.0 / duration.count()) << " gets/s\n";
    
    printStats(calculateStats(latencies), "获取连接延迟");
}

// ============================================================================
// 测试 2: 并发测试 - 多线程同时获取
// ============================================================================

void test_concurrent_gets() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试 2: 多线程并发获取 (" << config.concurrent_threads << " 线程)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    ConnectionPool pool(config.host, config.port, config.user,
                        config.password, config.database,
                        config.min_connections, config.max_connections,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    
    if (!pool.warmup()) {
        std::cerr << "连接池预热失败，跳过测试" << std::endl;
        return;
    }
    
    const int per_thread = 200;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    std::atomic<int64_t> total_latency_us{0};
    
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int t = 0; t < config.concurrent_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < per_thread; ++i) {
                auto t0 = std::chrono::high_resolution_clock::now();
                auto conn = pool.getConnection(std::chrono::seconds(5));
                auto t1 = std::chrono::high_resolution_clock::now();
                
                if (conn) {
                    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                    total_latency_us.fetch_add(latency);
                    pool.returnConnection(conn);
                    success_count++;
                } else {
                    fail_count++;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    int total_requests = success_count.load() + fail_count.load();
    
    std::cout << "总请求数: " << total_requests << "\n";
    std::cout << "成功: " << success_count.load() << "\n";
    std::cout << "失败: " << fail_count.load() << "\n";
    std::cout << "总耗时: " << duration.count() << " ms\n";
    std::cout << "吞吐量: " << (total_requests * 1000.0 / duration.count()) << " gets/s\n";
    
    if (success_count.load() > 0) {
        std::cout << "平均延迟: " << (total_latency_us.load() / 1000.0 / success_count.load()) << " ms\n";
    }
    
    std::cout << "池状态: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << "\n";
}

// ============================================================================
// 测试 3: 极限测试 - 超过连接池上限
// ============================================================================

void test_pool_exhaustion() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试 3: 连接池耗尽测试" << std::endl;
    std::cout << "========================================" << std::endl;
    
    ConnectionPool pool(config.host, config.port, config.user,
                        config.password, config.database,
                        5, 20,  // 较小的连接池
                        std::chrono::seconds(30), std::chrono::seconds(5));
    
    if (!pool.warmup()) {
        std::cerr << "连接池预热失败，跳过测试" << std::endl;
        return;
    }
    
    std::cout << "池大小: 最大=" << pool.totalConnections() << "\n";
    
    // 获取所有连接
    std::vector<ConnectionPtr> held_conns;
    std::atomic<int> acquired{0};
    
    std::thread acquirer([&]() {
        for (int i = 0; i < 100; ++i) {
            auto conn = pool.getConnection(std::chrono::milliseconds(100));
            if (conn) {
                held_conns.push_back(conn);
                acquired++;
            }
        }
    });
    
    acquirer.join();
    
    std::cout << "获取连接数: " << acquired.load() << "\n";
    std::cout << "池状态: 总=" << pool.totalConnections()
              << ", 空闲=" << pool.idleConnections()
              << ", 忙碌=" << pool.busyConnections() << "\n";
    
    // 释放连接
    std::thread releaser([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (auto& conn : held_conns) {
            pool.returnConnection(conn);
        }
        std::cout << "已释放所有连接\n";
    });
    
    releaser.join();
    
    // 验证连接可以重新获取
    auto conn = pool.getConnection(std::chrono::seconds(2));
    if (conn) {
        std::cout << "重新获取连接成功\n";
        pool.returnConnection(conn);
    } else {
        std::cout << "重新获取连接失败\n";
    }
}

// ============================================================================
// 测试 4: 长时间稳定性测试
// ============================================================================

void test_stability() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试 4: 稳定性测试 (" << config.test_duration_seconds << " 秒)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    ConnectionPool pool(config.host, config.port, config.user,
                        config.password, config.database,
                        config.min_connections, config.max_connections,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    
    if (!pool.warmup()) {
        std::cerr << "连接池预热失败，跳过测试" << std::endl;
        return;
    }
    
    std::atomic<bool> running{true};
    std::atomic<int> total_requests{0};
    std::atomic<int> failed_requests{0};
    
    // 工作线程
    std::vector<std::thread> workers;
    for (int t = 0; t < config.concurrent_threads; ++t) {
        workers.emplace_back([&]() {
            while (running.load()) {
                auto conn = pool.getConnection(std::chrono::seconds(2));
                if (conn) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    pool.returnConnection(conn);
                    total_requests++;
                } else {
                    failed_requests++;
                }
            }
        });
    }
    
    // 监控线程
    std::thread monitor([&]() {
        auto start = std::chrono::steady_clock::now();
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            
            std::cout << "[" << elapsed << "s] 请求="
                      << total_requests.load()
                      << ", 失败=" << failed_requests.load()
                      << ", 池: 总=" << pool.totalConnections()
                      << ", 空闲=" << pool.idleConnections()
                      << ", 忙碌=" << pool.busyConnections()
                      << "\n";
            
            if (elapsed >= config.test_duration_seconds) {
                running.store(false);
            }
        }
    });
    
    monitor.join();
    for (auto& t : workers) {
        t.join();
    }
    
    std::cout << "\n--- 最终统计 ---\n";
    std::cout << "总请求数: " << total_requests.load() << "\n";
    std::cout << "失败请求: " << failed_requests.load() << "\n";
    std::cout << "成功率: " << (100.0 * total_requests.load() / 
                (total_requests.load() + failed_requests.load())) << "%\n";
}

// ============================================================================
// 测试 5: 延迟分布测试
// ============================================================================

void test_latency_distribution() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试 5: 延迟分布测试 (1000 样本)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    ConnectionPool pool(config.host, config.port, config.user,
                        config.password, config.database,
                        config.min_connections, config.max_connections,
                        std::chrono::seconds(30), std::chrono::seconds(5));
    
    if (!pool.warmup()) {
        std::cerr << "连接池预热失败，跳过测试" << std::endl;
        return;
    }
    
    const int samples = 1000;
    std::vector<double> get_latencies;
    std::vector<double> return_latencies;
    get_latencies.reserve(samples);
    return_latencies.reserve(samples);
    
    for (int i = 0; i < samples; ++i) {
        // 测量获取延迟
        auto t0 = std::chrono::high_resolution_clock::now();
        auto conn = pool.getConnection(std::chrono::seconds(5));
        auto t1 = std::chrono::high_resolution_clock::now();
        
        if (conn) {
            get_latencies.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
            
            // 模拟使用
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            
            // 测量归还延迟
            auto t2 = std::chrono::high_resolution_clock::now();
            pool.returnConnection(conn);
            auto t3 = std::chrono::high_resolution_clock::now();
            
            return_latencies.push_back(
                std::chrono::duration<double, std::milli>(t3 - t2).count());
        }
    }
    
    std::cout << "\n获取连接延迟:\n";
    printStats(calculateStats(get_latencies), "Get Latency");
    
    std::cout << "\n归还连接延迟:\n";
    printStats(calculateStats(return_latencies), "Return Latency");
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    Logger::instance().init("", LogLevel::ERROR);
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << "用法: stress_test [选项]\n";
            std::cout << "选项:\n";
            std::cout << "  -h, --help              显示帮助\n";
            std::cout << "  -H, --host <地址>       MySQL 主机 (默认: 127.0.0.1)\n";
            std::cout << "  -P, --port <端口>       MySQL 端口 (默认: 3306)\n";
            std::cout << "  -u, --user <用户>       MySQL 用户 (默认: root)\n";
            std::cout << "  -p, --password <密码>   MySQL 密码 (默认: 空)\n";
            std::cout << "  -d, --database <库>    数据库名 (默认: test)\n";
            std::cout << "  -t, --threads <数量>    并发线程数 (默认: 50)\n";
            std::cout << "  -D, --duration <秒>    测试时长 (默认: 10)\n";
            return 0;
        }
        // ... 参数解析省略
    }
    
    std::cout << "========================================\n";
    std::cout << "       DB-Proxy 压力测试\n";
    std::cout << "========================================\n";
    std::cout << "配置:\n";
    std::cout << "  主机: " << config.host << ":" << config.port << "\n";
    std::cout << "  用户: " << config.user << "\n";
    std::cout << "  库: " << config.database << "\n";
    std::cout << "  连接池: " << config.min_connections << "-" << config.max_connections << "\n";
    std::cout << "  线程数: " << config.concurrent_threads << "\n";
    
    // 注意: 这些测试需要 MySQL 服务器运行才能正常工作
    // 以下是测试函数列表，实际运行需要取消注释
    
    // test_sequential_gets();
    // test_concurrent_gets();
    // test_pool_exhaustion();
    // test_stability();
    // test_latency_distribution();
    
    std::cout << "\n注意: 压力测试需要 MySQL 服务器运行。\n";
    std::cout << "请确保 MySQL 可用后，取消代码中的测试函数调用。\n";
    
    return 0;
}
