#ifndef DB_PROXY_CONNECTION_H
#define DB_PROXY_CONNECTION_H

#include "pool/backend_connection.h"
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <atomic>

namespace dbproxy {

/**
 * @brief MySQL 后端连接（握手、ping、简单查询与原始收发）
 *
 * 认证：`mysql_native_password`（内置 SHA-1）；`caching_sha2_password`（内置 SHA-256 SCRAMBLE + 可选 RSA-OAEP
 * 完整认证，完整认证需编译时定义 `DBPROXY_HAVE_OPENSSL` 并链接 OpenSSL）。
 * 套接字在池内为非阻塞 + `select`；`enterRawWireRelayMode` 将会话侧切为阻塞以便与透明中继配合。
 */
class Connection : public BackendConnection, public std::enable_shared_from_this<Connection> {
public:
    using State = BackendConnection::State;
    using Column = BackendConnection::Column;
    
    Connection(const std::string& host, uint16_t port,
               const std::string& user, const std::string& password,
               const std::string& database);
    ~Connection();
    
    // 连接管理
    bool connect() override;
    void close() override;
    bool isConnected() const override;
    
    // 健康检查
    bool ping() override;
    bool execute(const std::string& sql) override;
    bool sendRaw(const char* data, size_t len) override;
    ssize_t recvRaw(char* buffer, size_t len, std::chrono::milliseconds timeout) override;
    
    // 查询结果访问 (CLI 使用)
    const std::vector<Column>& resultColumns() const override { return result_columns_; }
    const std::vector<std::vector<std::string>>& resultRows() const override { return result_rows_; }
    int affectedRows() const override { return affected_rows_; }
    const std::string& lastError() const override { return last_error_; }
    void clearResult() override;
    
    // 状态
    State state() const override { return state_.load(); }
    void setState(State s) override { state_.store(s); }
    
    // 属性
    int fd() const override { return fd_; }
    uint64_t id() const override { return id_; }
    const std::string& remoteHost() const override { return remote_host_; }
    uint16_t remotePort() const override { return remote_port_; }
    BackendProtocol protocol() const override { return BackendProtocol::MySQL; }
    
    // 时间戳
    std::chrono::steady_clock::time_point lastActiveTime() const override { return last_active_; }
    void updateActiveTime() override { last_active_ = std::chrono::steady_clock::now(); }
    
    // 引用计数（用于连接池追踪）
    int refCount() const override { return ref_count_.load(); }
    void addRef() override { ref_count_.fetch_add(1); }
    void releaseRef() override { ref_count_.fetch_sub(1); }

    bool enterRawWireRelayMode() override;
    bool restoreSessionAfterRawRelay() override;

private:
    bool connectTcpOnly();
    void hardCloseSocketNoProtocol();

    bool doHandshake();
    bool recvResult();
    std::string makeAuthResponse(const std::string& scramble,
                                 const std::string& password);
    bool readPacket(std::string& payload, uint8_t* sequence_id = nullptr,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool writePacket(const std::string& payload, uint8_t sequence_id);

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
    
    // 查询结果
    std::vector<Column> result_columns_;
    std::vector<std::vector<std::string>> result_rows_;
    int affected_rows_{0};
    std::string last_error_;
    
    static uint64_t next_id_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_CONNECTION_H
