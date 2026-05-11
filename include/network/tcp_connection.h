#ifndef DB_PROXY_TCP_CONNECTION_H
#define DB_PROXY_TCP_CONNECTION_H

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace dbproxy {

/**
 * @brief TCP 连接封装
 * 
 * 面试亮点：
 * - 非阻塞 IO：使用 O_NONBLOCK 设置非阻塞模式
 * - 零拷贝思路：使用 buffer 减少内存拷贝
 * - 连接生命周期管理：引用计数
 * - write buffer 设计：应对对端处理慢的场景
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum class State {
        CONNECTING,
        CONNECTED,
        CLOSING,
        CLOSED
    };

    TcpConnection(int fd, const std::string& remote_ip, uint16_t remote_port);
    ~TcpConnection();
    
    int fd() const { return fd_; }
    /** 将 fd 交给调用方并在本对象中置为 -1，避免析构时 close；用于会话级透明中继。 */
    int releaseFd();
    State state() const { return state_; }
    const std::string& remoteIp() const { return remote_ip_; }
    uint16_t remotePort() const { return remote_port_; }
    
    // 接收数据
    ssize_t read(char* buffer, size_t len);
    
    // 发送数据
    ssize_t send(const char* data, size_t len);
    bool sendInLoop(const char* data, size_t len);
    
    // 连接状态
    void shutdown();
    void forceClose();
    void setState(State s) { state_ = s; }
    
    // 设置回调
    using MessageCallback = std::function<void(std::shared_ptr<TcpConnection>, const char*, size_t)>;
    using WriteCompleteCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
    using CloseCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
    
    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { on_write_complete_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { on_close_ = std::move(cb); }
    
    // epoll 相关
    bool isReading() const { return reading_; }
    void setReading(bool r) { reading_ = r; }
    bool isWriting() const { return !output_buffer_.empty(); }
    
    // 回调触发
    void handleRead();
    void handleWrite();
    void handleClose();

private:
    int fd_;
    State state_;
    std::string remote_ip_;
    uint16_t remote_port_;
    
    bool reading_{false};
    
    // 输入输出缓冲区
    // 面试亮点：使用 buffer 解耦读写，解决对端处理慢问题
    std::vector<char> input_buffer_;
    std::vector<char> output_buffer_;  // write buffer for backpressure
    
    MessageCallback on_message_;
    WriteCompleteCallback on_write_complete_;
    CloseCallback on_close_;
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

}  // namespace dbproxy

#endif  // DB_PROXY_TCP_CONNECTION_H
