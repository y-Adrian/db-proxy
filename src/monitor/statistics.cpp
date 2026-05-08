#include "monitor/statistics.h"
#include "core/logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace dbproxy {

Statistics::Statistics()
    : time_windows_(WINDOW_SIZE), last_window_time_(std::chrono::steady_clock::now()),
      start_time_(std::chrono::steady_clock::now()) {
}

Statistics& Statistics::instance() {
    static Statistics instance_;
    return instance_;
}

void Statistics::recordQuery(const std::string& sql, const std::string& type,
                            const std::string& database, uint64_t latency_ms) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 更新时间窗口
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_window_time_).count();
    
    if (elapsed >= 1) {
        current_window_ = (current_window_ + 1) % WINDOW_SIZE;
        time_windows_[current_window_] = TimeWindow{};
        last_window_time_ = now;
    }
    
    // 更新当前窗口
    time_windows_[current_window_].queries++;
    total_queries_++;
    
    if (type == "SELECT" || type == "SHOW" || type == "USE") {
        time_windows_[current_window_].reads++;
    } else {
        time_windows_[current_window_].writes++;
    }
    
    if (latency_ms > 100) {  // 假设 100ms 为慢查询阈值
        time_windows_[current_window_].errors++;
        slow_queries_++;
    }
    
    // 更新 SQL 统计
    SQLStats stats;
    stats.sql = sql;
    stats.type = type;
    stats.database = database;
    stats.count++;
    stats.total_latency_ms += latency_ms;
    stats.max_latency_ms = std::max(stats.max_latency_ms, latency_ms);
    stats.min_latency_ms = std::min(stats.min_latency_ms, latency_ms);
    stats.avg_latency_ms = static_cast<double>(stats.total_latency_ms) / stats.count;
    
    auto key = type + ":" + database;
    auto it = query_stats_.find(key);
    if (it == query_stats_.end()) {
        query_stats_[key] = stats;
    } else {
        it->second.count += stats.count;
        it->second.total_latency_ms += stats.total_latency_ms;
        it->second.max_latency_ms = std::max(it->second.max_latency_ms, stats.max_latency_ms);
        it->second.min_latency_ms = std::min(it->second.min_latency_ms, stats.min_latency_ms);
        it->second.avg_latency_ms = static_cast<double>(it->second.total_latency_ms) / it->second.count;
    }
}

std::vector<Statistics::SQLStats> Statistics::getSlowQueries(size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<SQLStats> result;
    for (const auto& [key, stats] : query_stats_) {
        if (stats.avg_latency_ms > 100) {  // 慢查询阈值
            result.push_back(stats);
        }
    }
    
    std::sort(result.begin(), result.end(), 
              [](const SQLStats& a, const SQLStats& b) {
                  return a.avg_latency_ms > b.avg_latency_ms;
              });
    
    if (result.size() > limit) {
        result.resize(limit);
    }
    
    return result;
}

std::vector<Statistics::SQLStats> Statistics::getTopQueries(size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<SQLStats> result;
    for (const auto& [key, stats] : query_stats_) {
        result.push_back(stats);
    }
    
    std::sort(result.begin(), result.end(),
              [](const SQLStats& a, const SQLStats& b) {
                  return a.count > b.count;
              });
    
    if (result.size() > limit) {
        result.resize(limit);
    }
    
    return result;
}

double Statistics::getQPS() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    int64_t total = 0;
    for (size_t i = 0; i < WINDOW_SIZE; ++i) {
        total += time_windows_[i].queries;
    }
    
    return static_cast<double>(total) / WINDOW_SIZE;
}

double Statistics::getReadQPS() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    int64_t total = 0;
    for (size_t i = 0; i < WINDOW_SIZE; ++i) {
        total += time_windows_[i].reads;
    }
    
    return static_cast<double>(total) / WINDOW_SIZE;
}

double Statistics::getWriteQPS() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    int64_t total = 0;
    for (size_t i = 0; i < WINDOW_SIZE; ++i) {
        total += time_windows_[i].writes;
    }
    
    return static_cast<double>(total) / WINDOW_SIZE;
}

void Statistics::setActiveConnections(int count) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    active_connections_ = count;
}

int Statistics::getActiveConnections() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return active_connections_;
}

void Statistics::recordClientQuery(const std::string& client_ip) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    client_stats_[client_ip]++;
    total_connections_++;
}

std::vector<std::pair<std::string, uint64_t>> Statistics::getTopClients(size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<std::pair<std::string, uint64_t>> result;
    for (const auto& [ip, count] : client_stats_) {
        result.emplace_back(ip, count);
    }
    
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.second > b.second;
              });
    
    if (result.size() > limit) {
        result.resize(limit);
    }
    
    return result;
}

Statistics::GlobalStats Statistics::getGlobalStats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    GlobalStats stats;
    stats.total_connections = total_connections_;
    stats.total_queries = total_queries_;
    stats.failed_queries = failed_queries_;
    stats.slow_queries = slow_queries_;
    stats.current_qps = getQPS();
    stats.current_rps = getReadQPS();
    stats.current_wps = getWriteQPS();
    stats.active_connections = active_connections_;
    stats.start_time = start_time_;
    stats.uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    
    return stats;
}

std::string Statistics::toJSON() const {
    std::ostringstream oss;
    auto stats = getGlobalStats();
    
    oss << "{\n";
    oss << "  \"total_connections\": " << stats.total_connections << ",\n";
    oss << "  \"total_queries\": " << stats.total_queries << ",\n";
    oss << "  \"failed_queries\": " << stats.failed_queries << ",\n";
    oss << "  \"slow_queries\": " << stats.slow_queries << ",\n";
    oss << "  \"current_qps\": " << std::fixed << std::setprecision(2) << stats.current_qps << ",\n";
    oss << "  \"current_rps\": " << stats.current_rps << ",\n";
    oss << "  \"current_wps\": " << stats.current_wps << ",\n";
    oss << "  \"active_connections\": " << stats.active_connections << ",\n";
    oss << "  \"uptime_seconds\": " << stats.uptime_seconds << "\n";
    oss << "}";
    
    return oss.str();
}

void Statistics::reset() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    query_stats_.clear();
    client_stats_.clear();
    time_windows_.assign(WINDOW_SIZE, TimeWindow{});
    total_connections_ = 0;
    total_queries_ = 0;
    failed_queries_ = 0;
    slow_queries_ = 0;
    active_connections_ = 0;
    start_time_ = std::chrono::steady_clock::now();
}

}  // namespace dbproxy
