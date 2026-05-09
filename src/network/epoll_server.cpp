#include "network/epoll_server.h"
#include "network/tcp_connection.h"
#include "network/event_loop.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <algorithm>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif

namespace dbproxy {

EpollServer::EpollServer()
    : listen_fd_(-1), epoll_fd_(-1), running_(false) {
    event_loop_ = std::make_unique<EventLoop>();
}

EpollServer::~EpollServer() {
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
}

bool EpollServer::listen(const std::string& host, uint16_t port) {
    // 创建监听 socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }
    
    // 设置 SO_REUSEADDR
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置非阻塞
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
    
    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind: " + std::string(strerror(errno)));
        return false;
    }
    
    // 监听
    if (::listen(listen_fd_, 128) < 0) {
        LOG_ERROR("Failed to listen: " + std::string(strerror(errno)));
        return false;
    }
    
    LOG_INFO("Server listening on " + host + ":" + std::to_string(port));
    return true;
}

void EpollServer::start() {
    running_ = true;

#if defined(__linux__)
    // 创建 epoll 实例
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("Failed to create epoll: " + std::string(strerror(errno)));
        return;
    }
    
    // 注册监听 socket 到 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // 边缘触发
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        LOG_ERROR("Failed to add listen fd to epoll: " + std::string(strerror(errno)));
        return;
    }
    
    LOG_INFO("Epoll server started");
    
    // 事件循环
    const int MAX_EVENTS = 1024;
    struct epoll_event events[MAX_EVENTS];
    
    while (running_) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;
            
            if (fd == listen_fd_) {
                // 处理新连接
                if (revents & EPOLLIN) {
                    handleNewConnection();
                }
            } else {
                // 处理已连接 socket
                auto it = connections_.find(fd);
                if (it != connections_.end()) {
                    if (revents & (EPOLLERR | EPOLLHUP)) {
                        it->second->handleClose();
                    } else {
                        if (revents & EPOLLIN) {
                            it->second->handleRead();
                        }
                        if (revents & EPOLLOUT) {
                            it->second->handleWrite();
                        }
                    }
                }
            }
        }
    }
#else
    LOG_INFO("Select fallback server started");

    while (running_) {
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        int max_fd = listen_fd_;
        if (listen_fd_ >= 0) {
            FD_SET(listen_fd_, &read_fds);
        }

        for (const auto& [fd, conn] : connections_) {
            if (fd < 0 || fd >= FD_SETSIZE) {
                continue;
            }
            FD_SET(fd, &read_fds);
            if (conn->isWriting()) {
                FD_SET(fd, &write_fds);
            }
            max_fd = std::max(max_fd, fd);
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int n = select(max_fd + 1, &read_fds, &write_fds, nullptr, &timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("Select failed: " + std::string(strerror(errno)));
            break;
        }
        if (n == 0) {
            continue;
        }

        if (listen_fd_ >= 0 && FD_ISSET(listen_fd_, &read_fds)) {
            handleNewConnection();
        }

        std::vector<int> ready_fds;
        ready_fds.reserve(connections_.size());
        for (const auto& [fd, _] : connections_) {
            ready_fds.push_back(fd);
        }

        for (int fd : ready_fds) {
            auto it = connections_.find(fd);
            if (it == connections_.end()) {
                continue;
            }
            if (FD_ISSET(fd, &read_fds)) {
                it->second->handleRead();
            }
            it = connections_.find(fd);
            if (it != connections_.end() && FD_ISSET(fd, &write_fds)) {
                it->second->handleWrite();
            }
        }
    }
#endif
}

void EpollServer::handleNewConnection() {
    // 边缘触发模式需要循环接受所有连接
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 没有更多连接
            }
            LOG_ERROR("Accept failed: " + std::string(strerror(errno)));
            break;
        }
        
        // 设置非阻塞
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        // 设置 TCP_NODELAY
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        // 获取客户端地址
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        uint16_t client_port = ntohs(client_addr.sin_port);
        
        // 创建连接对象
        auto conn = std::make_shared<TcpConnection>(client_fd, client_ip, client_port);
        
        // 设置回调
        conn->setMessageCallback(on_message_);
        conn->setCloseCallback([this](auto c) {
            connections_.erase(c->fd());
            if (on_close_) on_close_(c);
        });
        
#if defined(__linux__)
        // 注册到 epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = client_fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);
#endif
        
        connections_[client_fd] = conn;
        
        LOG_DEBUG("New connection from " + std::string(client_ip) + ":" + std::to_string(client_port));
        
        // 回调
        if (on_connection_) {
            on_connection_(conn);
        }
    }
}

void EpollServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

int EpollServer::getActiveConnectionCount() const {
    return static_cast<int>(connections_.size());
}

}  // namespace dbproxy
