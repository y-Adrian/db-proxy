#ifndef DB_PROXY_POOL_MANAGER_H
#define DB_PROXY_POOL_MANAGER_H

#include "pool/connection_pool.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace dbproxy {

/**
 * @brief 连接池管理器 - 支持多数据库
 * 
 * 面试亮点：
 * - 读写锁保护：读多写少场景优化
 * - 数据库分片支持
 * - 连接路由：根据 SQL 类型选择只读/读写池
 */
class PoolManager {
public:
    static PoolManager& instance();
    
    // 添加数据库连接池
    bool addPool(const std::string& name,
                 const std::string& host, uint16_t port,
                 const std::string& user, const std::string& password,
                 const std::string& database,
                 size_t min_connections = 5,
                 size_t max_connections = 50,
                 BackendProtocol protocol = BackendProtocol::MySQL);
    
    // 获取连接池
    std::shared_ptr<ConnectionPool> getPool(const std::string& name);
    
    // 获取连接
    ConnectionPtr getConnection(const std::string& pool_name, 
                                std::chrono::milliseconds timeout = std::chrono::seconds(5));
    
    // 归还连接
    void returnConnection(const std::string& pool_name, ConnectionPtr conn);
    
    // 路由选择（返回默认池，可扩展为读写分离）
    std::shared_ptr<ConnectionPool> routePool();
    
    // 健康检查所有连接池
    void healthCheckAll();
    
    // 关闭所有连接池
    void shutdownAll();
    
    // 统计
    struct PoolStats {
        std::string name;
        size_t total;
        size_t idle;
        size_t busy;
    };
    std::vector<PoolStats> getAllStats() const;

private:
    PoolManager() = default;
    ~PoolManager() { shutdownAll(); }
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ConnectionPool>> pools_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_POOL_MANAGER_H
