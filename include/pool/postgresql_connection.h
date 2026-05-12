#ifndef DB_PROXY_POSTGRESQL_CONNECTION_H
#define DB_PROXY_POSTGRESQL_CONNECTION_H

#include "pool/backend_connection.h"
#include <atomic>

namespace dbproxy {

/** PostgreSQL 后端连接；认证支持 trust / 明文 / MD5 / SCRAM-SHA-256（实现依赖 OpenSSL）。池内非阻塞 IO，`enterRawWireRelayMode` 切阻塞供透明中继。 */
class PostgreSQLConnection : public BackendConnection,
                             public std::enable_shared_from_this<PostgreSQLConnection> {
public:
    using State = BackendConnection::State;
    using Column = BackendConnection::Column;

    PostgreSQLConnection(const std::string& host, uint16_t port,
                         const std::string& user, const std::string& password,
                         const std::string& database);
    ~PostgreSQLConnection() override;

    bool connect() override;
    void close() override;
    bool isConnected() const override;
    bool ping() override;
    bool execute(const std::string& sql) override;
    bool sendRaw(const char* data, size_t len) override;
    ssize_t recvRaw(char* buffer, size_t len, std::chrono::milliseconds timeout) override;

    const std::vector<Column>& resultColumns() const override { return result_columns_; }
    const std::vector<std::vector<std::string>>& resultRows() const override { return result_rows_; }
    int affectedRows() const override { return affected_rows_; }
    const std::string& lastError() const override { return last_error_; }
    void clearResult() override;

    State state() const override { return state_.load(); }
    void setState(State s) override { state_.store(s); }

    int fd() const override { return fd_; }
    uint64_t id() const override { return id_; }
    const std::string& remoteHost() const override { return remote_host_; }
    uint16_t remotePort() const override { return remote_port_; }
    BackendProtocol protocol() const override { return BackendProtocol::PostgreSQL; }

    std::chrono::steady_clock::time_point lastActiveTime() const override { return last_active_; }
    void updateActiveTime() override { last_active_ = std::chrono::steady_clock::now(); }

    int refCount() const override { return ref_count_.load(); }
    void addRef() override { ref_count_.fetch_add(1); }
    void releaseRef() override { ref_count_.fetch_sub(1); }

    bool enterRawWireRelayMode() override;
    bool restoreSessionAfterRawRelay() override;

private:
    bool connectTcpOnly();
    void hardCloseSocketNoProtocol();

    bool doStartup();
    bool handleAuthentication(int32_t auth_type, const std::string& payload);
    bool handleCleartextPassword();
    bool handleMD5Password(const char* salt);
    bool handleSASL(const std::string& payload);
    bool readMessage(char& type, std::string& payload,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool writeMessage(char type, const std::string& payload);
    bool waitReadyForQuery();
    bool parseQueryMessage(char type, const std::string& payload);
    bool readExact(char* buffer, size_t len, std::chrono::milliseconds timeout);
    bool writeAll(const char* data, size_t len);
    bool waitReadable(std::chrono::milliseconds timeout) const;
    bool waitWritable(std::chrono::milliseconds timeout) const;

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

    std::vector<Column> result_columns_;
    std::vector<std::vector<std::string>> result_rows_;
    int affected_rows_{0};
    std::string last_error_;

    static uint64_t next_id_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_POSTGRESQL_CONNECTION_H
