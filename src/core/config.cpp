#include "core/config.h"
#include <fstream>
#include <sstream>

namespace dbproxy {

Config loadConfig() {
    Config config;
    
    // 简化实现：使用默认值
    // 实际项目中应从 YAML/JSON 配置文件读取
    
    // 服务配置
    config.server.host = "0.0.0.0";
    config.server.port = 3306;
    config.server.max_connections = 10000;
    config.server.worker_threads = 4;
    config.server.backlog = 128;
    
    // 连接池配置
    config.pool.min_connections = 10;
    config.pool.max_connections = 100;
    config.pool.max_idle_time_ms = 30000;
    config.pool.connection_timeout_ms = 5000;
    config.pool.health_check_interval_ms = 10000;
    
    // 示例数据库配置
    DatabaseConfig db;
    db.host = "127.0.0.1";
    db.port = 3306;
    db.username = "root";
    db.password = "";
    db.database = "test";
    db.charset = "utf8mb4";
    config.databases.push_back(db);
    
    // 监控配置
    config.monitoring.enable = true;
    config.monitoring.metrics_interval_ms = 1000;
    config.monitoring.slow_query_threshold_ms = 100;
    
    // 日志配置
    config.log_level = "INFO";
    
    return config;
}

}  // namespace dbproxy
