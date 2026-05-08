#ifndef DB_PROXY_METRICS_H
#define DB_PROXY_METRICS_H

#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <string>
#include <functional>

namespace dbproxy {

/**
 * @brief 指标收集器
 * 
 * 面试亮点：
 * - 无锁计数器：使用原子变量减少锁竞争
 * - 滑动窗口：计算 QPS/TPS
 * - 延迟直方图：P50/P90/P99/P999
 * - 可观测性：Prometheus 格式输出
 */
class Metrics {
public:
    static Metrics& instance();
    
    // 计数器
    void incrementCounter(const std::string& name);
    void incrementCounter(const std::string& name, int64_t delta);
    int64_t getCounter(const std::string& name) const;
    
    // 仪表盘（gauge）
    void setGauge(const std::string& name, double value);
    void incrementGauge(const std::string& name);
    void decrementGauge(const std::string& name);
    double getGauge(const std::string& name) const;
    
    // 延迟记录（用于计算 P50/P90/P99）
    void recordLatency(const std::string& name, std::chrono::milliseconds ms);
    
    // 直方图数据
    struct HistogramData {
        int64_t count{0};
        double sum{0};
        double min{0};
        double max{0};
        double p50{0};
        double p90{0};
        double p99{0};
        double p999{0};
    };
    HistogramData getHistogram(const std::string& name) const;
    
    // 重置
    void reset();
    
    // 输出 Prometheus 格式
    std::string toPrometheusFormat() const;
    
private:
    Metrics() = default;
    ~Metrics() = default;
    
    struct Counter {
        std::atomic<int64_t> value{0};
    };
    
    struct Gauge {
        std::atomic<double> value{0};
    };
    
    struct LatencyBucket {
        int64_t le;  // less than or equal
        std::atomic<int64_t> count{0};
    };
    
    struct Histogram {
        std::atomic<int64_t> count{0};
        std::atomic<double> sum{0};
        std::atomic<double> min{std::numeric_limits<double>::max()};
        std::atomic<double> max{0};
        
        // 滑动窗口 bucket（简化实现）
        static constexpr int BUCKETS = 11;
        LatencyBucket buckets[BUCKETS] = {
            {1, 0}, {5, 0}, {10, 0}, {50, 0}, {100, 0},
            {500, 0}, {1000, 0}, {5000, 0}, {10000, 0}, {30000, 0}, {60000, 0}
        };
        
        void record(double value);
        HistogramData getData() const;
    };
    
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
    
    mutable std::shared_mutex mutex_;
};

// 全局便捷宏
#define METRICS_INC(name) dbproxy::Metrics::instance().incrementCounter(name)
#define METRICS_INC_BY(name, v) dbproxy::Metrics::instance().incrementCounter(name, v)
#define METRICS_GAUGE_SET(name, v) dbproxy::Metrics::instance().setGauge(name, v)
#define METRICS_LATENCY(name, ms) dbproxy::Metrics::instance().recordLatency(name, ms)

}  // namespace dbproxy

#endif  // DB_PROXY_METRICS_H
