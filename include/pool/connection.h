#ifndef DB_PROXY_CONNECTION_H
#define DB_PROXY_CONNECTION_H

#include <cstdint>
#include <string>
#include <chrono>
#include <memory>
#include <atomic>

namespace dbproxy {

/**
 * @brief 数据库连接封装
 * 
 * 面试亮点：
 * - 连接生命周期管理：创建、校验、心跳、归还
 * - 智能指针 + 引用计数：防止连接泄漏
 * - 非阻塞 IO：使用 poll/select 检测连接状态
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    enum class State {
        IDLE,           // 空闲
        IN_USE,         // 使用中
        TESTING,        // 健康检查中
        CLOSED          // 已关闭
    };
    
    Connection(const std::string& host, uint16_t port,
               const std::string& user, const std::string& password,
               const std::string& database);
    ~Connection();
    
    // 连接管理
    bool connect();
    void close();
    bool isConnected() const;
    
    // 健康检查
    bool ping();
    bool execute(const std::string& sql);
    
    // 状态
    State state() const { return state_.load(); }
    void setState(State s) { state_.store(s); }
    
    // 属性
    int fd() const { return fd_; }
    uint64_t id() const { return id_; }
    const std::string& remoteHost() const { return remote_host_; }
    uint16_t remotePort() const { return remote_port_; }
    
    // 时间戳
    std::chrono::steady_clock::time_point lastActiveTime() const { return last_active_; }
    void updateActiveTime() { last_active_ = std::chrono::steady_clock::now(); }
    
    // 引用计数（用于连接池追踪）
    int refCount() const { return ref_count_.load(); }
    void addRef() { ref_count_.fetch_add(1); }
    void releaseRef() { ref_count_.fetch_sub(1); }
    
private:
    int fd_{-1};
    uint64_t id_;
    
    std::string remote_host_;
    uint16_t remote_port_;
    std::string username_;
    std::string password_;
    std::string database_;
    
    std::atomic<State> state_{State::IDLE};
    std::atomic<int> ref_count_{0};
    std::chrono::steady_clock::time_point last_active_;
    
    static uint64_t next_id_;
};

using ConnectionPtr = std::shared_ptr<Connection>;

}  // namespace dbproxy

#endif  // DB_PROXY_CONNECTION_H
