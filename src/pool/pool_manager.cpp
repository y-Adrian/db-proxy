#include "pool/pool_manager.h"
#include "core/logger.h"
#include <chrono>

namespace dbproxy {

PoolManager& PoolManager::instance() {
    static PoolManager instance_;
    return instance_;
}

bool PoolManager::addPool(const std::string& name,
                         const std::string& host, uint16_t port,
                         const std::string& user, const std::string& password,
                         const std::string& database,
                         size_t min_connections,
                         size_t max_connections,
                         BackendProtocol protocol) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (pools_.find(name) != pools_.end()) {
        LOG_WARN("Pool already exists: " + name);
        return false;
    }
    
    auto pool = std::make_shared<ConnectionPool>(
        host, port, user, password, database,
        min_connections, max_connections,
        std::chrono::milliseconds(30000),
        std::chrono::milliseconds(5000),
        protocol
    );
    
    if (!pool->warmup()) {
        LOG_ERROR("Failed to warmup pool: " + name);
        return false;
    }
    
    pools_[name] = pool;
    LOG_INFO("Pool added: " + name);
    return true;
}

std::shared_ptr<ConnectionPool> PoolManager::getPool(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pools_.find(name);
    if (it != pools_.end()) {
        return it->second;
    }
    return nullptr;
}

ConnectionPtr PoolManager::getConnection(const std::string& pool_name,
                                         std::chrono::milliseconds timeout) {
    auto pool = getPool(pool_name);
    if (!pool) {
        LOG_ERROR("Pool not found: " + pool_name);
        return nullptr;
    }
    
    return pool->getConnection(timeout);
}

void PoolManager::returnConnection(const std::string& pool_name, ConnectionPtr conn) {
    auto pool = getPool(pool_name);
    if (pool) {
        pool->returnConnection(conn);
    }
}

std::shared_ptr<ConnectionPool> PoolManager::routePool(const std::string& sql_type) {
    // 简单路由：默认返回第一个池
    // 实际应该根据 SQL 类型、数据库等选择池
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (!pools_.empty()) {
        return pools_.begin()->second;
    }
    
    return nullptr;
}

void PoolManager::healthCheckAll() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (auto& [name, pool] : pools_) {
        pool->healthCheck();
    }
}

void PoolManager::shutdownAll() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& [name, pool] : pools_) {
        pool->shutdownAll();
        LOG_INFO("Pool shutdown: " + name);
    }
    pools_.clear();
}

std::vector<PoolManager::PoolStats> PoolManager::getAllStats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<PoolStats> result;
    
    for (const auto& [name, pool] : pools_) {
        PoolStats stats;
        stats.name = name;
        stats.total = pool->totalConnections();
        stats.idle = pool->idleConnections();
        stats.busy = pool->busyConnections();
        result.push_back(stats);
    }
    
    return result;
}

}  // namespace dbproxy
