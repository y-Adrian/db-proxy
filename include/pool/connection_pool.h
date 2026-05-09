#ifndef DB_PROXY_CONNECTION_POOL_H
#define DB_PROXY_CONNECTION_POOL_H

#include "pool/connection.h"
#include "pool/backend_connection.h"
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace dbproxy {

/**
 * @brief 数据库连接池
 * 
 * 面试亮点：
 * - 生产者-消费者模式：多线程安全
 * - 连接预热：启动时创建最小连接数
 * - 连接复用：减少连接创建开销
 * - 条件变量 + 互斥锁：高效线程同步
 * - 超时机制：防止无限等待
 */
class ConnectionPool {
public:
    ConnectionPool(const std::string& host, uint16_t port,
                  const std::string& user, const std::string& password,
                  const std::string& database,
                  size_t min_connections,
                  size_t max_connections,
                  std::chrono::milliseconds max_idle_time,
                  std::chrono::milliseconds connection_timeout,
                  BackendProtocol protocol = BackendProtocol::MySQL);
    
    ~ConnectionPool();
    
    // 禁止拷贝
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    
    // 获取连接（带超时）
    ConnectionPtr getConnection(std::chrono::milliseconds timeout);
    
    // 归还连接
    void returnConnection(ConnectionPtr conn);
    
    // 关闭连接（从池中移除）
    void removeConnection(ConnectionPtr conn);
    
    // 健康检查
    void healthCheck();
    
    // 清理空闲连接
    void cleanIdleConnections();

    // 关闭连接池，唤醒等待线程
    void shutdownAll();
    void close();
    
    // 预热：创建最小连接数
    bool warmup();
    
    // 状态
    size_t totalConnections() const { return total_connections_.load(); }
    size_t idleConnections() const { return idle_connections_.load(); }
    size_t busyConnections() const { return busy_connections_.load(); }
    
    void setMaxIdleTime(std::chrono::milliseconds ms) { max_idle_time_ = ms; }
    void setConnectionTimeout(std::chrono::milliseconds ms) { connection_timeout_ = ms; }
    
private:
    ConnectionPtr createConnection();
    void destroyConnection(ConnectionPtr conn);
    void expandPool();  // 扩容
    void shrinkPool();  // 缩容
    
    std::string host_;
    uint16_t port_;
    std::string username_;
    std::string password_;
    std::string database_;
    BackendProtocol protocol_;
    
    size_t min_connections_;
    size_t max_connections_;
    std::chrono::milliseconds max_idle_time_;
    std::chrono::milliseconds connection_timeout_;
    
    std::atomic<size_t> total_connections_{0};
    std::atomic<size_t> idle_connections_{0};
    std::atomic<size_t> busy_connections_{0};
    
    // 连接队列
    std::queue<ConnectionPtr> idle_queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    bool closed_{false};
    
    // 统计
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> failed_connections_{0};
};

}  // namespace dbproxy

#endif  // DB_PROXY_CONNECTION_POOL_H
