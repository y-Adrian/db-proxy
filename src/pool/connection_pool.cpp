#include "pool/connection_pool.h"
#include "core/logger.h"
#include <chrono>

#if DBPROXY_ENABLE_POSTGRES
#include "pool/postgresql_connection.h"
#endif

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

    std::lock_guard<std::mutex> lock(mutex_);
    while (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        idle_connections_.fetch_sub(1);
        destroyConnection(conn);
    }
}

ConnectionPtr ConnectionPool::createConnection() {
    ConnectionPtr conn;
    if (protocol_ == BackendProtocol::PostgreSQL) {
#if DBPROXY_ENABLE_POSTGRES
        conn = std::make_shared<PostgreSQLConnection>(host_, port_, username_, password_, database_);
#else
        LOG_ERROR("PostgreSQL support is disabled in this build (missing OpenSSL)");
        return nullptr;
#endif
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
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        if (closed_) {
            return nullptr;
        }

        // 1. 尝试从空闲队列获取
        if (!idle_queue_.empty()) {
            auto conn = idle_queue_.front();
            idle_queue_.pop();
            idle_connections_.fetch_sub(1);

            if (conn->isConnected() && conn->ping()) {
                conn->setState(Connection::State::IN_USE);
                busy_connections_.fetch_add(1);
                conn->updateActiveTime();
                return conn;
            }
            destroyConnection(conn);
            continue;
        }

        // 2. 尝试创建新连接
        const size_t total = total_connections_.load();
        if (total < max_connections_) {
            lock.unlock();
            auto conn = createConnection();
            lock.lock();
            if (closed_) {
                if (conn) {
                    destroyConnection(conn);
                }
                return nullptr;
            }
            if (conn) {
                // 并发创建可能短暂超过 max；销毁多余连接，保证上限
                if (total_connections_.load() > max_connections_) {
                    destroyConnection(conn);
                    continue;
                }
                conn->setState(Connection::State::IN_USE);
                busy_connections_.fetch_add(1);
                return conn;
            }
            continue;
        }

        // 3. 等待空闲连接或关闭
        if (std::chrono::steady_clock::now() >= deadline) {
            total_requests_.fetch_add(1);
            LOG_WARN("Connection pool timeout");
            return nullptr;
        }

        cv_.wait_until(lock, deadline, [this] {
            return closed_ || !idle_queue_.empty();
        });
    }
}

void ConnectionPool::returnConnection(ConnectionPtr conn) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    busy_connections_.fetch_sub(1);

    if (closed_) {
        destroyConnection(conn);
        return;
    }

    if (conn->isConnected()) {
        conn->setState(Connection::State::IDLE);
        idle_connections_.fetch_add(1);
        idle_queue_.push(conn);
        conn->updateActiveTime();
        cv_.notify_one();
    } else {
        destroyConnection(conn);
    }
}

void ConnectionPool::removeConnection(ConnectionPtr conn) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    bool found_in_idle = false;
    std::queue<ConnectionPtr> new_queue;
    while (!idle_queue_.empty()) {
        auto c = idle_queue_.front();
        idle_queue_.pop();
        if (c != conn) {
            new_queue.push(c);
        } else {
            found_in_idle = true;
            idle_connections_.fetch_sub(1);
            destroyConnection(c);
        }
    }
    idle_queue_ = std::move(new_queue);

    if (!found_in_idle) {
        busy_connections_.fetch_sub(1);
        destroyConnection(conn);
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
