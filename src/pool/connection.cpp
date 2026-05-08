#include "pool/connection.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>
#include <fcntl.h>

namespace dbproxy {

uint64_t Connection::next_id_ = 1;

Connection::Connection(const std::string& host, uint16_t port,
                     const std::string& user, const std::string& password,
                     const std::string& database)
    : id_(next_id_++), remote_host_(host), remote_port_(port),
      username_(user), password_(password), database_(database),
      last_active_(std::chrono::steady_clock::now()) {
}

Connection::~Connection() {
    close();
}

bool Connection::connect() {
    // 创建 socket
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERROR("Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }
    
    // 设置非阻塞
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    
    // 解析地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port_);
    
    // 支持域名和 IP
    if (inet_pton(AF_INET, remote_host_.c_str(), &addr.sin_addr) <= 0) {
        // 尝试 DNS 解析
        struct hostent* he = gethostbyname(remote_host_.c_str());
        if (he == nullptr) {
            LOG_ERROR("Failed to resolve host: " + remote_host_);
            close();
            return false;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    // 连接
    int ret = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        LOG_ERROR("Failed to connect: " + std::string(strerror(errno)));
        close();
        return false;
    }
    
    // 简化：等待连接完成
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);
    struct timeval tv = {5, 0};  // 5 秒超时
    
    ret = select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
    if (ret <= 0) {
        LOG_ERROR("Connection timeout or error");
        close();
        return false;
    }
    
    // 检查连接状态
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        LOG_ERROR("Connection failed: " + std::string(strerror(error)));
        close();
        return false;
    }
    
    state_.store(State::IDLE);
    LOG_DEBUG("Connection " + std::to_string(id_) + " connected to " + 
              remote_host_ + ":" + std::to_string(remote_port_));
    return true;
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    state_.store(State::CLOSED);
}

bool Connection::isConnected() const {
    return fd_ >= 0 && state_.load() != State::CLOSED;
}

bool Connection::ping() {
    // 简化实现：检查 fd 是否有效
    if (!isConnected()) {
        return false;
    }
    
    // 实际应该发送 COM_PING
    return true;
}

bool Connection::execute(const std::string& sql) {
    // 简化实现
    if (!isConnected()) {
        return false;
    }
    
    // 实际应该发送 SQL
    // 这里简化处理
    updateActiveTime();
    return true;
}

}  // namespace dbproxy
