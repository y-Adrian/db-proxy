#include "network/tcp_connection.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

namespace dbproxy {

TcpConnection::TcpConnection(int fd, const std::string& remote_ip, uint16_t remote_port)
    : fd_(fd), state_(State::CONNECTED), remote_ip_(remote_ip), remote_port_(remote_port) {
    input_buffer_.reserve(64 * 1024);  // 64KB 预分配
    output_buffer_.reserve(64 * 1024);
}

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

ssize_t TcpConnection::read(char* buffer, size_t len) {
    ssize_t n = ::read(fd_, buffer, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // 非阻塞，无数据
        }
        LOG_ERROR("Read error: " + std::string(strerror(errno)));
        return -1;
    }
    return n;
}

ssize_t TcpConnection::send(const char* data, size_t len) {
    ssize_t n = ::send(fd_, data, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // 发送缓冲区满
        }
        LOG_ERROR("Send error: " + std::string(strerror(errno)));
        return -1;
    }
    return n;
}

bool TcpConnection::sendInLoop(const char* data, size_t len) {
    // 如果没有正在发送的数据，直接发送
    if (output_buffer_.empty()) {
        ssize_t n = send(data, len);
        if (n < 0) {
            return false;
        }
        
        if (static_cast<size_t>(n) < len) {
            // 部分发送，剩余放入 buffer
            output_buffer_.insert(output_buffer_.end(), data + n, data + len);
        }
    } else {
        // 正在发送中，数据放入 buffer
        output_buffer_.insert(output_buffer_.end(), data, data + len);
    }
    return true;
}

void TcpConnection::shutdown() {
    if (state_ == State::CONNECTED) {
        ::shutdown(fd_, SHUT_WR);
        state_ = State::CLOSING;
    }
}

void TcpConnection::forceClose() {
    if (state_ != State::CLOSED) {
        state_ = State::CLOSED;
        close(fd_);
        fd_ = -1;
    }
}

void TcpConnection::handleRead() {
    // 读取数据直到 EAGAIN
    char buffer[16 * 1024];
    
    while (true) {
        ssize_t n = read(buffer, sizeof(buffer));
        if (n > 0) {
            // 数据追加到输入 buffer
            input_buffer_.insert(input_buffer_.end(), buffer, buffer + n);
            
            // 如果有完整的消息，触发回调
            if (on_message_ && !input_buffer_.empty()) {
                // 简化处理：将所有数据传给上层
                // 实际应该解析完整的 MySQL 包
                on_message_(shared_from_this(), input_buffer_.data(), input_buffer_.size());
                input_buffer_.clear();
            }
        } else if (n == 0) {
            // 对端关闭连接
            LOG_DEBUG("Connection closed by peer");
            handleClose();
            return;
        } else {
            // 错误
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Read error: " + std::string(strerror(errno)));
                handleClose();
            }
            return;
        }
    }
}

void TcpConnection::handleWrite() {
    // 发送 output_buffer_ 中的数据
    while (!output_buffer_.empty()) {
        ssize_t n = send(output_buffer_.data(), output_buffer_.size());
        if (n > 0) {
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + n);
        } else if (n == 0) {
            // 发送缓冲区满，等待下一次 EPOLLOUT
            return;
        } else {
            // 错误
            LOG_ERROR("Write error: " + std::string(strerror(errno)));
            handleClose();
            return;
        }
    }
    
    // 数据发送完毕
    if (on_write_complete_) {
        on_write_complete_(shared_from_this());
    }
}

void TcpConnection::handleClose() {
    state_ = State::CLOSED;
    if (on_close_) {
        on_close_(shared_from_this());
    }
}

}  // namespace dbproxy
