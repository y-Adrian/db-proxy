#include "pool/connection_pool.h"
#include "pool/postgresql_connection.h"
#include "core/logger.h"
#include <chrono>

namespace dbproxy {

ConnectionPool::ConnectionPool(const std::string& host, uint16_t port,
                               const std::string& user, const std::string& password,
                               const std::string& database,
                               size_t min_connections,
                               size_t max_connections,
                               std::chrono::milliseconds max_idle_time,
                               std::chrono::milliseconds connection_timeout,
                               BackendProtocol protocol)
    : host_(host), port_(port), username_(user), password_(password), database_(database),
      protocol_(protocol), min_connections_(min_connections), max_connections_(max_connections),
      max_idle_time_(max_idle_time), connection_timeout_(connection_timeout) {
    
    LOG_INFO("Creating connection pool: " + host_ + ":" + std::to_string(port_) + 
             " min=" + std::to_string(min_connections_) + 
             " max=" + std::to_string(max_connections_));
}

ConnectionPool::~ConnectionPool() {
    close();
    
    // 销毁所有连接
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        conn->close();
    }
}

ConnectionPtr ConnectionPool::createConnection() {
    ConnectionPtr conn;
    if (protocol_ == BackendProtocol::PostgreSQL) {
        conn = std::make_shared<PostgreSQLConnection>(host_, port_, username_, password_, database_);
    } else {
        conn = std::make_shared<Connection>(host_, port_, username_, password_, database_);
    }
    
    if (conn->connect()) {
        total_connections_.fetch_add(1);
        return conn;
    }
    
    failed_connections_.fetch_add(1);
    LOG_ERROR("Failed to create connection");
    return nullptr;
}

void ConnectionPool::destroyConnection(ConnectionPtr conn) {
    if (conn) {
        conn->close();
        total_connections_.fetch_sub(1);
    }
}

ConnectionPtr ConnectionPool::getConnection(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    while (true) {
        // 1. 尝试从空闲队列获取
        if (!idle_queue_.empty()) {
            auto conn = idle_queue_.front();
            idle_queue_.pop();
            idle_connections_.fetch_sub(1);
            
            // 检查连接是否有效
            if (conn->isConnected() && conn->ping()) {
                conn->setState(Connection::State::IN_USE);
                busy_connections_.fetch_add(1);
                conn->updateActiveTime();
                return conn;
            } else {
                // 连接已失效，销毁
                destroyConnection(conn);
                continue;
            }
        }
        
        // 2. 尝试创建新连接
        size_t total = total_connections_.load();
        if (total < max_connections_) {
            lock.unlock();
            auto conn = createConnection();
            if (conn) {
                lock.lock();
                conn->setState(Connection::State::IN_USE);
                busy_connections_.fetch_add(1);
                return conn;
            }
            lock.lock();
            continue;
        }
        
        // 3. 等待空闲连接
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            // 超时
            total_requests_.fetch_add(1);
            LOG_WARN("Connection pool timeout");
            return nullptr;
        }
        
        // 等待条件变量
        cv_.wait_for(lock, remaining);
    }
}

void ConnectionPool::returnConnection(ConnectionPtr conn) {
    if (!conn) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    busy_connections_.fetch_sub(1);
    
    // 检查连接是否有效
    if (conn->isConnected()) {
        conn->setState(Connection::State::IDLE);
        idle_connections_.fetch_add(1);
        idle_queue_.push(conn);
        conn->updateActiveTime();
        
        // 唤醒等待的获取者
        cv_.notify_one();
    } else {
        // 连接已失效，销毁
        destroyConnection(conn);
    }
}

void ConnectionPool::removeConnection(ConnectionPtr conn) {
    if (!conn) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 从空闲队列中移除
    std::queue<ConnectionPtr> new_queue;
    while (!idle_queue_.empty()) {
        auto c = idle_queue_.front();
        idle_queue_.pop();
        if (c != conn) {
            new_queue.push(c);
        } else {
            idle_connections_.fetch_sub(1);
            destroyConnection(c);
        }
    }
    idle_queue_ = std::move(new_queue);
    
    // 如果仍在使用中，标记为已关闭
    if (conn->state() != Connection::State::CLOSED) {
        conn->close();
    }
}

void ConnectionPool::healthCheck() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::queue<ConnectionPtr> new_queue;
    
    while (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        
        if (!conn->isConnected() || !conn->ping()) {
            // 连接失效，销毁
            idle_connections_.fetch_sub(1);
            destroyConnection(conn);
        } else {
            new_queue.push(conn);
        }
    }
    
    idle_queue_ = std::move(new_queue);
    
    // 补充空闲连接
    while (total_connections_.load() < min_connections_) {
        auto conn = createConnection();
        if (conn) {
            conn->setState(Connection::State::IDLE);
            idle_connections_.fetch_add(1);
            idle_queue_.push(conn);
        } else {
            break;
        }
    }
}

void ConnectionPool::cleanIdleConnections() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    std::queue<ConnectionPtr> new_queue;
    
    while (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        
        auto idle_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - conn->lastActiveTime()).count();
        
        if (idle_time > max_idle_time_.count() && 
            total_connections_.load() > min_connections_) {
            // 超时且有多余连接，销毁
            idle_connections_.fetch_sub(1);
            destroyConnection(conn);
        } else {
            new_queue.push(conn);
        }
    }
    
    idle_queue_ = std::move(new_queue);
}

bool ConnectionPool::warmup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (size_t i = 0; i < min_connections_; ++i) {
        auto conn = createConnection();
        if (conn) {
            conn->setState(Connection::State::IDLE);
            idle_connections_.fetch_add(1);
            idle_queue_.push(conn);
        } else {
            LOG_WARN("Failed to create min connection " + std::to_string(i + 1));
            return false;
        }
    }
    
    LOG_INFO("Connection pool warmed up: " + std::to_string(min_connections_) + " connections");
    return true;
}

void ConnectionPool::shutdownAll() {
    closed_ = true;
    cv_.notify_all();
}

void ConnectionPool::close() {
    shutdownAll();
}

}  // namespace dbproxy
