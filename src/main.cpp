/**
 * @file main.cpp
 * @brief 数据库代理中间件 - 主程序入口
 * 
 * 功能：
 * - 高性能 TCP 服务器（基于 epoll）
 * - MySQL 协议解析
 * - 连接池管理
 * - 性能监控
 */

#include "network/epoll_server.h"
#include "network/tcp_connection.h"
#include "pool/pool_manager.h"
#include "monitor/metrics.h"
#include "monitor/statistics.h"
#include "monitor/performance_analyzer.h"
#include "core/logger.h"
#include "core/config.h"
#include "protocol/mysql_parser.h"

#include <memory>
#include <csignal>
#include <thread>
#include <chrono>

using namespace dbproxy;

// 全局服务器指针，用于信号处理
std::unique_ptr<EpollServer> g_server;

void signalHandler(int sig) {
    LOG_INFO("Received signal " + std::to_string(sig) + ", shutting down...");
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    // 初始化日志（使用项目本地目录，避免 /var/log 权限问题）
    Logger::instance().init("./logs/proxy.log", LogLevel::INFO);
    LOG_INFO("=== Database Proxy Starting ===");

    // 加载配置
    Config config;
    config.server.host = "0.0.0.0";
    config.server.port = 6033;  // 代理监听端口，避免与后端 MySQL 3306 冲突
    config.server.max_connections = 10000;

    // 添加数据库连接池（连接后端 MySQL 3306）
    PoolManager::instance().addPool(
        "default",
        "127.0.0.1", 3306,
        "root", "",
        "test",
        10,  // min connections
        100  // max connections
    );
    
    // 创建服务器
    g_server = std::make_unique<EpollServer>();
    
    if (!g_server->listen(config.server.host, config.server.port)) {
        LOG_ERROR("Failed to start server");
        return 1;
    }
    
    // 设置连接回调
    g_server->setConnectionCallback([](auto conn) {
        LOG_DEBUG("New connection: " + conn->remoteIp() + ":" + std::to_string(conn->remotePort()));
        METRICS_INC("connections_total");
        METRICS_GAUGE_SET("active_connections", 
                         Metrics::instance().getGauge("active_connections") + 1);
    });
    
    // 设置消息回调
    g_server->setMessageCallback([](auto conn, const char* data, size_t len) {
        // 简化：直接转发到后端
        METRICS_INC("queries_total");
        
        auto start = std::chrono::steady_clock::now();
        
        // 获取连接池
        auto pool = PoolManager::instance().getConnection("default", 
            std::chrono::milliseconds(5000));
        
        if (!pool) {
            LOG_ERROR("Failed to get connection from pool");
            return;
        }
        
        // 执行查询（这里简化处理）
        // 实际应该解析 SQL 并正确处理
        
        // 归还连接
        PoolManager::instance().returnConnection("default", pool);
        
        auto end = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        METRICS_LATENCY("query_latency", latency);
        METRICS_GAUGE_SET("avg_latency_ms", latency.count());
    });
    
    // 设置关闭回调
    g_server->setCloseCallback([](auto conn) {
        LOG_DEBUG("Connection closed: " + conn->remoteIp());
        METRICS_GAUGE_SET("active_connections",
                         Metrics::instance().getGauge("active_connections") - 1);
    });
    
    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // 启动健康检查线程
    std::thread health_check([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            PoolManager::instance().healthCheckAll();
            PerformanceAnalyzer::instance().collectSnapshot();
        }
    });
    health_check.detach();
    
    // 启动统计输出线程
    std::thread stats_printer([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            auto stats = Statistics::instance().getGlobalStats();
            auto pools = PoolManager::instance().getAllStats();
            
            LOG_INFO("=== Stats ===");
            LOG_INFO("QPS: " + std::to_string(stats.current_qps));
            LOG_INFO("Active Connections: " + std::to_string(stats.active_connections));
            
            for (const auto& pool : pools) {
                LOG_INFO("Pool [" + pool.name + "]: total=" + 
                         std::to_string(pool.total) + 
                         " idle=" + std::to_string(pool.idle) +
                         " busy=" + std::to_string(pool.busy));
            }
        }
    });
    stats_printer.detach();
    
    // 启动服务器
    LOG_INFO("Server listening on " + config.server.host + ":" + std::to_string(config.server.port));
    g_server->start();
    
    // 清理
    PoolManager::instance().shutdownAll();
    
    LOG_INFO("=== Database Proxy Stopped ===");
    return 0;
}
