#ifndef DB_PROXY_CONFIG_H
#define DB_PROXY_CONFIG_H

#include <string>
#include <vector>
#include <cstdint>

namespace dbproxy {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 3306;
    int max_connections = 10000;
    int worker_threads = 4;
    int backlog = 128;
};

struct PoolConfig {
    int min_connections = 10;
    int max_connections = 100;
    int max_idle_time_ms = 30000;
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
};

struct MonitoringConfig {
    bool enable = true;
    int metrics_interval_ms = 1000;
    int slow_query_threshold_ms = 100;
    bool enable_query_logging = false;
};

struct Config {
    ServerConfig server;
    PoolConfig pool;
    std::vector<DatabaseConfig> databases;
    MonitoringConfig monitoring;
    std::string log_level = "INFO";
    std::string log_file = "./logs/proxy.log";
};

Config loadConfig(const std::string& config_file);

}  // namespace dbproxy

#endif  // DB_PROXY_CONFIG_H
