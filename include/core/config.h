#ifndef DB_PROXY_CONFIG_H
#define DB_PROXY_CONFIG_H

#include <string>
#include <vector>
#include <cstdint>

namespace dbproxy {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 3306;
    /** 预留：当前 EpollServer 未用于 accept 限流 */
    int max_connections = 10000;
    /** 客户端会话工作线程数（每会话在线程内阻塞透传） */
    int worker_threads = 4;
    /** 预留：listen backlog 尚未从配置读取 */
    int backlog = 128;
};

struct PoolConfig {
    int min_connections = 10;
    /** 池内连接上限，即主程序可同时透传的最大客户端会话数 */
    int max_connections = 100;
    int max_idle_time_ms = 30000;
    /** 从池借连接的最大等待时间（毫秒），非 SQL 执行超时 */
    int connection_timeout_ms = 5000;
    int health_check_interval_ms = 10000;
    bool enable_connection_reuse = true;
};

struct DatabaseConfig {
    std::string host;
    uint16_t port = 3306;
    std::string username;
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    /** 线协议：mysql（默认）| postgresql | postgres | pg（池内握手；主程序对该后端字节透传） */
    std::string protocol = "mysql";
};

/** 监控：`enable` 总开关；`metrics_port>0` 时在本机提供 GET /metrics（Prometheus 文本） */
struct MonitoringConfig {
    bool enable = true;
    /** 统计日志与池指标同步间隔（毫秒） */
    int metrics_interval_ms = 1000;
    /** 透明代理下表示「单条客户端会话持续时间」阈值（毫秒），超过记慢会话 WARN */
    int slow_query_threshold_ms = 100;
    /** 为 true 时每个代理会话结束打一条 INFO（无 SQL 文本） */
    bool enable_query_logging = false;
    /** 绑定 Prometheus /metrics 的地址；metrics_port=0 时不监听 */
    std::string metrics_host = "127.0.0.1";
    /** 0 表示关闭 HTTP 指标；例如 19100 */
    uint16_t metrics_port = 0;
};

struct Config {
    ServerConfig server;
    PoolConfig pool;
    std::vector<DatabaseConfig> databases;
    MonitoringConfig monitoring;
    std::string log_level = "INFO";
    std::string log_file = "./logs/proxy.log";
};

// Load config from INI file; falls back to built-in defaults if file is
// absent or unreadable.
Config loadConfig(const std::string& config_file = "proxy.conf");

}  // namespace dbproxy

#endif  // DB_PROXY_CONFIG_H
