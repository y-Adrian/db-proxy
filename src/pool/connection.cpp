#include "pool/connection.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <cstring>
#include <random>
#include <iomanip>
#include <sstream>
#include <openssl/ssl.h>
#include <openssl/err.h>

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
    
    if (inet_pton(AF_INET, remote_host_.c_str(), &addr.sin_addr) <= 0) {
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
    
    // 等待连接完成
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);
    struct timeval tv = {5, 0};
    
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
    
    // MySQL 握手协议
    if (!doHandshake()) {
        LOG_ERROR("MySQL handshake failed");
        close();
        return false;
    }
    
    state_.store(State::IDLE);
    LOG_DEBUG("Connection " + std::to_string(id_) + " connected to " + 
              remote_host_ + ":" + std::to_string(remote_port_));
    return true;
}

bool Connection::doHandshake() {
    // 1. 接收服务端握手包
    char buffer[1024];
    ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        return false;
    }
    
    // 解析握手包
    // 协议版本
    uint8_t protocol_version = buffer[0];
    if (protocol_version != 0x0a) {
        LOG_ERROR("Unsupported protocol version: " + std::to_string(protocol_version));
        return false;
    }
    
    // 服务器版本（null结尾）
    size_t offset = 1;
    while (offset < n && buffer[offset] != 0) offset++;
    std::string server_version(buffer + 1, buffer + offset);
    offset++;  // skip null
    
    // 连接 ID
    uint32_t connection_id = *reinterpret_cast<uint32_t*>(buffer + offset);
    offset += 4;
    
    // auth-plugin-data-part-1 (8 bytes)
    std::string auth_plugin_data(buffer + offset, buffer + offset + 8);
    offset += 8 + 1;  // +1 for filler
    
    // Capabilities (2 bytes first part)
    uint32_t capabilities = *reinterpret_cast<uint16_t*>(buffer + offset);
    offset += 2;
    
    // Character set + status flags
    uint8_t charset = buffer[offset++];
    uint16_t status_flags = *reinterpret_cast<uint16_t*>(buffer + offset);
    offset += 2;
    
    // Capabilities (2 bytes second part)
    capabilities |= (uint32_t)buffer[offset++] << 16;
    capabilities |= (uint32_t)buffer[offset++] << 24;
    
    // auth-plugin-data-length
    uint8_t auth_plugin_len = buffer[offset++];
    
    // Reserved (10 bytes)
    offset += 10;
    
    // auth-plugin-data-part-2
    std::string auth_plugin_data_part2;
    if (offset + auth_plugin_len - 8 > 0) {
        auth_plugin_data_part2.assign(buffer + offset, buffer + offset + auth_plugin_len - 8);
    }
    
    // 完整的 auth-data
    std::string auth_data = auth_plugin_data + auth_plugin_data_part2;
    
    // 2. 发送认证包
    // 计算 auth-plugin-data 的 scramble
    std::string scramble = auth_data;
    
    // 计算密码哈希
    std::string auth_response = makeAuthResponse(scramble, password_);
    
    // 构建认证包
    std::vector<char> auth_packet;
    
    // Capabilities
    uint32_t client_cap = 0
        | 0x0002  // CLIENT_LONG_PASSWORD
        | 0x0008  // CLIENT_FOUND_ROWS
        | 0x0020  // CLIENT_IGNORE_SIGPIPE
        | 0x0100  // CLIENT_IGNORE_SPACE
        | 0x8000  // CLIENT_PROTOCOL_41
        | 0x20000 // CLIENT_SECURE_CONNECTION
        | 0x40000000; // CLIENT_MULTI_STATEMENTS
    
    // 长度计算
    // 4 bytes capabilities
    // 4 bytes max_packet_size
    // 1 byte charset
    // 23 bytes reserved
    // n bytes username (null-terminated)
    // n bytes auth-response-length + auth-response
    // n bytes database (null-terminated)
    
    size_t packet_size = 4 + 4 + 1 + 23 + username_.size() + 1 + 
                         (auth_response.empty() ? 1 : auth_response.size() + 1) +
                         (database_.empty() ? 0 : database_.size() + 1);
    
    auth_packet.reserve(4 + packet_size);
    
    // 包长度
    auth_packet.push_back((packet_size >> 0) & 0xff);
    auth_packet.push_back((packet_size >> 8) & 0xff);
    auth_packet.push_back((packet_size >> 16) & 0xff);
    auth_packet.push_back(1);  // sequence ID
    
    // Capabilities
    auth_packet.push_back((client_cap >> 0) & 0xff);
    auth_packet.push_back((client_cap >> 8) & 0xff);
    auth_packet.push_back((client_cap >> 16) & 0xff);
    auth_packet.push_back((client_cap >> 24) & 0xff);
    
    // Max packet size
    auth_packet.push_back(0x00);
    auth_packet.push_back(0x00);
    auth_packet.push_back(0x00);
    auth_packet.push_back(0x00);
    
    // Charset
    auth_packet.push_back(charset);
    
    // Reserved
    for (int i = 0; i < 23; i++) auth_packet.push_back(0);
    
    // Username
    auth_packet.insert(auth_packet.end(), username_.begin(), username_.end());
    auth_packet.push_back(0);
    
    // Auth response
    if (auth_response.empty()) {
        auth_packet.push_back(0);
    } else {
        auth_packet.push_back((char)auth_response.size());
        auth_packet.insert(auth_packet.end(), auth_response.begin(), auth_response.end());
    }
    
    // Database
    if (!database_.empty()) {
        auth_packet.insert(auth_packet.end(), database_.begin(), database_.end());
        auth_packet.push_back(0);
    }
    
    // 发送认证包
    ssize_t sent = send(fd_, auth_packet.data(), auth_packet.size(), 0);
    if (sent != (ssize_t)auth_packet.size()) {
        LOG_ERROR("Failed to send auth packet");
        return false;
    }
    
    // 3. 接收认证结果
    n = recv(fd_, buffer, 1, 0);
    if (n <= 0) {
        return false;
    }
    
    // 检查响应类型
    uint8_t packet_type = buffer[0];
    if (packet_type == 0xff) {
        // Error packet
        recv(fd_, buffer + 1, 8, 0);  // skip header
        LOG_ERROR("Authentication failed");
        return false;
    } else if (packet_type == 0xfe) {
        // EOF packet (old auth, unlikely)
        LOG_WARN("Old authentication method");
        return false;
    } else if (packet_type == 0x00) {
        // OK packet
        LOG_DEBUG("Authentication successful");
    }
    
    // 如果服务器请求挑战响应（通过 auth plugin）
    // 这是一个简化实现，实际生产环境需要更完整的处理
    
    return true;
}

std::string Connection::makeAuthResponse(const std::string& scramble, 
                                          const std::string& password) {
    if (password.empty()) {
        return "";
    }
    
    // SHA1(password)
    unsigned char hash1[20];
    SHA1((const unsigned char*)password.c_str(), password.size(), hash1);
    
    // SHA1(scramble)
    unsigned char hash2[20];
    SHA1((const unsigned char*)scramble.c_str(), scramble.size(), hash2);
    
    // SHA1(hash1 XOR hash2)
    unsigned char result[20];
    for (int i = 0; i < 20; i++) {
        result[i] = hash1[i] ^ hash2[i];
    }
    
    // 验证：服务端应该计算 SHA1(SHA1(password)) 并与 stored_auth 比较
    // 这里简化处理，直接返回 token
    
    return std::string((char*)result, 20);
}

void Connection::close() {
    if (fd_ >= 0) {
        // 发送 COM_QUIT
        char quit_packet[5] = {1, 0x01, 0x00, 0x00, 0x00};
        send(fd_, quit_packet, 4, 0);
        
        ::close(fd_);
        fd_ = -1;
    }
    state_.store(State::CLOSED);
}

bool Connection::isConnected() const {
    return fd_ >= 0 && state_.load() != State::CLOSED;
}

bool Connection::ping() {
    if (!isConnected()) {
        return false;
    }
    
    // 发送 COM_PING
    char ping[5] = {1, 0x0e, 0x00, 0x00, 0x00};
    ssize_t n = send(fd_, ping, 4, 0);
    if (n != 4) {
        return false;
    }
    
    // 接收响应
    char buffer[10];
    n = recv(fd_, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        return false;
    }
    
    return buffer[0] == 0x00;  // OK packet
}

bool Connection::execute(const std::string& sql) {
    if (!isConnected()) {
        last_error_ = "Not connected";
        return false;
    }
    
    // 清空之前的结果
    clearResult();
    
    // 发送 COM_QUERY
    std::vector<char> packet;
    packet.push_back(sql.size() + 1);  // length
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(0x03);  // COM_QUERY
    packet.insert(packet.end(), sql.begin(), sql.end());
    
    ssize_t n = send(fd_, packet.data(), packet.size(), 0);
    if (n != (ssize_t)packet.size()) {
        last_error_ = "Failed to send query";
        return false;
    }
    
    // 接收响应
    return recvResult();
}

void Connection::clearResult() {
    result_columns_.clear();
    result_rows_.clear();
    affected_rows_ = 0;
    last_error_.clear();
}

bool Connection::recvResult() {
    char buffer[4096];
    
    // 接收包头
    ssize_t n = recv(fd_, buffer, 4, 0);
    if (n != 4) {
        last_error_ = "Failed to receive packet header";
        return false;
    }
    
    size_t packet_len = buffer[0] | (buffer[1] << 8) | (buffer[2] << 16);
    uint8_t seq_id = buffer[3];
    
    if (packet_len == 0) {
        return true;  // 空响应
    }
    
    // 接收包体
    std::string data;
    size_t received = 0;
    while (received < packet_len) {
        size_t to_recv = std::min((size_t)4096, packet_len - received);
        n = recv(fd_, buffer, to_recv, 0);
        if (n <= 0) {
            last_error_ = "Connection closed during receive";
            return false;
        }
        data.append(buffer, n);
        received += n;
    }
    
    uint8_t packet_type = data[0];
    
    if (packet_type == 0x00) {
        // OK packet - 查询成功
        size_t offset = 1;
        
        // 解析 affected rows (LENENC)
        if (offset < data.size() && data[offset] < 0xfb) {
            affected_rows_ = (int)data[offset++];
        }
        
        return true;
    } else if (packet_type == 0xff) {
        // Error packet
        if (data.size() > 4) {
            uint16_t error_code = data[1] | (data[2] << 8);
            std::string msg = data.substr(9);  // skip #XXXXX
            last_error_ = "Error " + std::to_string(error_code) + ": " + msg;
        } else {
            last_error_ = "Unknown error";
        }
        LOG_ERROR("Query error: {}", last_error_);
        return false;
    } else if (packet_type == 0xfb) {
        // LOCAL INFILE
        LOG_WARN("Local infile not supported");
        last_error_ = "Local infile not supported";
        return false;
    } else {
        // 结果集（SELECT等）
        // 简化处理：只获取列信息
        size_t offset = 0;
        
        // 跳过列数 (LENENC)
        if (offset < data.size()) {
            uint8_t first = data[offset++];
            if (first == 0xfc && offset + 2 <= data.size()) {
                offset += 2;
            } else if (first == 0xfd && offset + 3 <= data.size()) {
                offset += 3;
            } else if (first == 0xfe && offset + 8 <= data.size()) {
                offset += 8;
            }
        }
        
        // 添加一个默认列
        result_columns_.push_back({"result", "text"});
        result_rows_.push_back({data.substr(1)});  // 简化：整行作为一个值
        
        LOG_DEBUG("Result set received");
        return true;
    }
}

}  // namespace dbproxy
