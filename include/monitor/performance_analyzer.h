#ifndef DB_PROXY_PERFORMANCE_ANALYZER_H
#define DB_PROXY_PERFORMANCE_ANALYZER_H

#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <functional>

namespace dbproxy {

/**
 * @brief 性能分析器
 * 
 * 面试亮点：
 * - 性能瓶颈定位：网络延迟 vs 数据库延迟
 * - 容量规划：基于 QPS/连接数预测
 * - 告警机制：异常检测
 */
class PerformanceAnalyzer {
public:
    static PerformanceAnalyzer& instance();
    
    // 性能指标
    struct PerformanceMetrics {
        double cpu_usage{0};
        double memory_usage{0};
        double network_throughput_mbps{0};
        int active_connections{0};
        double query_per_second{0};
        double avg_latency_ms{0};
        double p99_latency_ms{0};
        int connection_pool_usage{0};  // 百分比
    };
    
    // 获取当前性能指标
    PerformanceMetrics getCurrentMetrics();
    
    // 瓶颈分析
    struct BottleneckReport {
        enum class Type {
            NONE,
            CPU_BOUND,
            MEMORY_BOUND,
            NETWORK_BOUND,
            DATABASE_BOUND,
            CONNECTION_POOL_EXHAUSTED
        };
        Type type{Type::NONE};
        double severity{0};  // 0-100
        std::string description;
        std::vector<std::string> recommendations;
    };
    
    BottleneckReport analyze();
    
    // 性能趋势
    struct TrendPoint {
        std::chrono::steady_clock::time_point time;
        double value;
    };
    
    std::vector<TrendPoint> getLatencyTrend(std::chrono::minutes duration);
    std::vector<TrendPoint> getQPSTrend(std::chrono::minutes duration);
    
    // 告警
    struct Alert {
        enum class Level {
            INFO,
            WARNING,
            CRITICAL
        };
        Level level;
        std::string message;
        std::chrono::steady_clock::time_point time;
    };
    
    using AlertCallback = std::function<void(Alert)>;
    void setAlertCallback(AlertCallback cb);
    std::vector<Alert> getRecentAlerts(size_t limit = 10);
    
    // 历史数据收集（用于趋势分析）
    void collectSnapshot();
    
private:
    PerformanceAnalyzer();
    
    struct Snapshot {
        std::chrono::steady_clock::time_point time;
        double latency_ms;
        double qps;
        int connections;
        double cpu;
        double memory;
    };
    
    std::vector<Snapshot> snapshots_;
    static constexpr size_t MAX_SNAPSHOTS = 3600;  // 保留 1 小时
    
    AlertCallback alert_callback_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_PERFORMANCE_ANALYZER_H
