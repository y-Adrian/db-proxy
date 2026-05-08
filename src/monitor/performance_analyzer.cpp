#include "monitor/performance_analyzer.h"
#include "core/logger.h"
#include "monitor/metrics.h"
#include "monitor/statistics.h"
#include <algorithm>
#include <chrono>

namespace dbproxy {

PerformanceAnalyzer::PerformanceAnalyzer() {
    snapshots_.reserve(MAX_SNAPSHOTS);
}

PerformanceAnalyzer& PerformanceAnalyzer::instance() {
    static PerformanceAnalyzer instance_;
    return instance_;
}

PerformanceAnalyzer::PerformanceMetrics PerformanceAnalyzer::getCurrentMetrics() {
    PerformanceMetrics metrics;
    
    // 从 Metrics 获取
    metrics.query_per_second = Metrics::instance().getGauge("qps");
    metrics.avg_latency_ms = Metrics::instance().getGauge("avg_latency_ms");
    metrics.p99_latency_ms = Metrics::instance().getHistogram("query_latency").p99;
    metrics.active_connections = static_cast<int>(Metrics::instance().getGauge("active_connections"));
    metrics.connection_pool_usage = static_cast<int>(Metrics::instance().getGauge("pool_usage_percent"));
    
    // 从 Statistics 获取
    auto stats = Statistics::instance().getGlobalStats();
    metrics.query_per_second = stats.current_qps;
    metrics.active_connections = stats.active_connections;
    
    // CPU/内存（简化实现）
    metrics.cpu_usage = 0.0;
    metrics.memory_usage = 0.0;
    
    return metrics;
}

PerformanceAnalyzer::BottleneckReport PerformanceAnalyzer::analyze() {
    BottleneckReport report;
    auto metrics = getCurrentMetrics();
    
    // 检查各项指标
    if (metrics.cpu_usage > 80) {
        report.type = BottleneckReport::Type::CPU_BOUND;
        report.severity = metrics.cpu_usage;
        report.description = "CPU 使用率过高: " + std::to_string(metrics.cpu_usage) + "%";
        report.recommendations.push_back("考虑增加工作线程数");
        report.recommendations.push_back("检查是否有 CPU 密集型操作");
    }
    
    if (metrics.p99_latency_ms > 1000) {
        report.type = BottleneckReport::Type::DATABASE_BOUND;
        report.severity = std::min(100.0, metrics.p99_latency_ms / 10);
        report.description = "P99 延迟过高: " + std::to_string(metrics.p99_latency_ms) + "ms";
        report.recommendations.push_back("检查数据库慢查询");
        report.recommendations.push_back("考虑添加索引");
        report.recommendations.push_back("考虑读写分离");
    }
    
    if (metrics.connection_pool_usage > 90) {
        report.type = BottleneckReport::Type::CONNECTION_POOL_EXHAUSTED;
        report.severity = metrics.connection_pool_usage;
        report.description = "连接池使用率过高: " + std::to_string(metrics.connection_pool_usage) + "%";
        report.recommendations.push_back("增加连接池大小");
        report.recommendations.push_back("检查连接泄漏");
        report.recommendations.push_back("考虑连接池分片");
    }
    
    return report;
}

std::vector<PerformanceAnalyzer::TrendPoint> PerformanceAnalyzer::getLatencyTrend(
    std::chrono::minutes duration) {
    std::vector<TrendPoint> result;
    
    auto cutoff = std::chrono::steady_clock::now() - duration;
    
    for (const auto& snapshot : snapshots_) {
        if (snapshot.time >= cutoff) {
            result.push_back({snapshot.time, snapshot.latency_ms});
        }
    }
    
    return result;
}

std::vector<PerformanceAnalyzer::TrendPoint> PerformanceAnalyzer::getQPSTrend(
    std::chrono::minutes duration) {
    std::vector<TrendPoint> result;
    
    auto cutoff = std::chrono::steady_clock::now() - duration;
    
    for (const auto& snapshot : snapshots_) {
        if (snapshot.time >= cutoff) {
            result.push_back({snapshot.time, snapshot.qps});
        }
    }
    
    return result;
}

void PerformanceAnalyzer::setAlertCallback(AlertCallback cb) {
    alert_callback_ = std::move(cb);
}

std::vector<PerformanceAnalyzer::Alert> PerformanceAnalyzer::getRecentAlerts(size_t limit) {
    // 简化实现
    return {};
}

void PerformanceAnalyzer::collectSnapshot() {
    auto metrics = getCurrentMetrics();
    
    Snapshot snapshot;
    snapshot.time = std::chrono::steady_clock::now();
    snapshot.latency_ms = metrics.avg_latency_ms;
    snapshot.qps = metrics.query_per_second;
    snapshot.connections = metrics.active_connections;
    snapshot.cpu = metrics.cpu_usage;
    snapshot.memory = metrics.memory_usage;
    
    snapshots_.push_back(snapshot);
    
    // 保持固定大小
    if (snapshots_.size() > MAX_SNAPSHOTS) {
        snapshots_.erase(snapshots_.begin());
    }
    
    // 检查是否需要告警
    auto report = analyze();
    if (report.severity > 80) {
        Alert alert;
        alert.level = report.severity > 95 ? Alert::Level::CRITICAL : Alert::Level::WARNING;
        alert.message = report.description;
        alert.time = std::chrono::steady_clock::now();
        
        if (alert_callback_) {
            alert_callback_(alert);
        }
    }
}

}  // namespace dbproxy
