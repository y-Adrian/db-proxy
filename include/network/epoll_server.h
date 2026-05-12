#ifndef DB_PROXY_EPOLL_SERVER_H
#define DB_PROXY_EPOLL_SERVER_H

#include <cstdint>
#include <functional>
#include <vector>
#include <unordered_map>
#include <memory>

namespace dbproxy {

class EventLoop;
class TcpConnection;

/**
 * @brief 监听侧 TCP 服务器（Linux: epoll；其它平台: select 回退）
 *
 * - Linux：监听与已接入连接注册 epoll，边缘触发 (ET) 读事件。
 * - 非 Linux：select 轮询；与 Linux 路径在「客户端 fd 管理方式」上略有差异。
 * - 新连接建立后由 setConnectionCallback 处理；主程序中通常会 takeConnection 后交给工作线程。
 */
class EpollServer {
public:
    using ConnectionCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
    using MessageCallback = std::function<void(std::shared_ptr<TcpConnection>, const char*, size_t)>;
    using CloseCallback = std::function<void(std::shared_ptr<TcpConnection>)>;

    EpollServer();
    ~EpollServer();
    
    bool listen(const std::string& host, uint16_t port);
    void setConnectionCallback(ConnectionCallback cb) { on_connection_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { on_close_ = std::move(cb); }
    
    void start();
    void stop();
    
    int getActiveConnectionCount() const;

    /** 从多路复用中摘除该 fd（不再由本 Server 驱动读写），返回连接对象；用于工作线程接管。 */
    std::shared_ptr<TcpConnection> takeConnection(int fd);

private:
    void handleNewConnection();
    void updateChannel(int fd, uint32_t events);
    
    int listen_fd_;
    int epoll_fd_;
    bool running_;
    
    std::unique_ptr<EventLoop> event_loop_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    
    ConnectionCallback on_connection_;
    MessageCallback on_message_;
    CloseCallback on_close_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_EPOLL_SERVER_H
