#ifndef DB_PROXY_STATISTICS_H
#define DB_PROXY_STATISTICS_H

#include <cstdint>
#include <atomic>
#include <vector>
#include <chrono>
#include <string>
#include <unordered_map>
#include <shared_mutex>

namespace dbproxy {

/**
 * @brief 统计信息收集器
 * 
 * 面试亮点：
 * - 多维度统计：SQL 类型、数据库、客户端 IP
 * - 时间窗口：滑动窗口计算
 * - 慢查询分析
 */
class Statistics {
public:
    static Statistics& instance();
    
    // SQL 统计
    struct SQLStats {
        std::string sql;
        std::string type;
        std::string database;
        uint64_t count{0};
        uint64_t total_latency_ms{0};
        uint64_t max_latency_ms{0};
        uint64_t min_latency_ms{UINT64_MAX};
        double avg_latency_ms{0};
    };
    
    void recordQuery(const std::string& sql, const std::string& type,
                     const std::string& database, uint64_t latency_ms);

    /** 透明代理：会话结束（无 SQL）；参与 QPS 窗口与慢会话（按 duration）统计 */
    void recordRelaySessionEnd(const std::string& client_ip, uint64_t duration_ms,
                               uint64_t slow_session_threshold_ms);
    
    // 获取 Top N 慢查询
    std::vector<SQLStats> getSlowQueries(size_t limit = 10) const;
    
    // 获取 Top N QPS
    std::vector<SQLStats> getTopQueries(size_t limit = 10) const;
    
    // QPS 计算（滑动窗口）
    double getQPS() const;
    double getReadQPS() const;
    double getWriteQPS() const;
    
    // 活跃连接数
    void setActiveConnections(int count);
    int getActiveConnections() const;
    
    // 客户端统计
    void recordClientQuery(const std::string& client_ip);
    std::vector<std::pair<std::string, uint64_t>> getTopClients(size_t limit = 5) const;
    
    // 全局统计
    struct GlobalStats {
        uint64_t total_connections{0};
        uint64_t total_queries{0};
        uint64_t failed_queries{0};
        uint64_t slow_queries{0};
        double current_qps{0};
        double current_rps{0};  // read queries per second
        double current_wps{0};  // write queries per second
        int active_connections{0};
        std::chrono::steady_clock::time_point start_time;
        uint64_t uptime_seconds{0};
    };
    GlobalStats getGlobalStats() const;
    
    // 输出 JSON 格式
    std::string toJSON() const;
    
    void reset();
    
private:
    Statistics();
    
    // 时间窗口（滑动窗口，窗口大小 1 秒）
    struct TimeWindow {
        uint64_t queries{0};
        uint64_t reads{0};
        uint64_t writes{0};
        uint64_t errors{0};
    };
    
    static constexpr size_t WINDOW_SIZE = 60;  // 60 秒窗口
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SQLStats> query_stats_;
    std::unordered_map<std::string, uint64_t> client_stats_;
    std::vector<TimeWindow> time_windows_;
    size_t current_window_{0};
    std::chrono::steady_clock::time_point last_window_time_;
    int active_connections_{0};
    uint64_t total_connections_{0};
    uint64_t total_queries_{0};
    uint64_t failed_queries_{0};
    uint64_t slow_queries_{0};
    std::chrono::steady_clock::time_point start_time_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_STATISTICS_H
