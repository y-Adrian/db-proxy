#ifndef DB_PROXY_BACKEND_CONNECTION_H
#define DB_PROXY_BACKEND_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace dbproxy {

enum class BackendProtocol {
    MySQL,
    PostgreSQL
};

class BackendConnection {
public:
    enum class State {
        IDLE,
        IN_USE,
        TESTING,
        CLOSED
    };

    struct Column {
        std::string name;
        std::string type;
    };

    virtual ~BackendConnection() = default;

    virtual bool connect() = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
    virtual bool ping() = 0;
    virtual bool execute(const std::string& sql) = 0;

    virtual bool sendRaw(const char* data, size_t len) = 0;
    virtual ssize_t recvRaw(char* buffer, size_t len, std::chrono::milliseconds timeout) = 0;

    virtual const std::vector<Column>& resultColumns() const = 0;
    virtual const std::vector<std::vector<std::string>>& resultRows() const = 0;
    virtual int affectedRows() const = 0;
    virtual const std::string& lastError() const = 0;
    virtual void clearResult() = 0;

    virtual State state() const = 0;
    virtual void setState(State s) = 0;

    virtual int fd() const = 0;
    virtual uint64_t id() const = 0;
    virtual const std::string& remoteHost() const = 0;
    virtual uint16_t remotePort() const = 0;
    virtual BackendProtocol protocol() const = 0;

    virtual std::chrono::steady_clock::time_point lastActiveTime() const = 0;
    virtual void updateActiveTime() = 0;

    virtual int refCount() const = 0;
    virtual void addRef() = 0;
    virtual void releaseRef() = 0;

    /**
     * 关闭当前应用层会话，与后端建立一条仅 TCP（未走 DB 握手）的连接，
     * 用于主代理路径上把客户端线协议原样透传到数据库。
     * 成功后可对 fd() 与客户端做字节级双向中继；结束时应调用 restoreSessionAfterRawRelay()。
     */
    virtual bool enterRawWireRelayMode() = 0;

    /** 结束透传后恢复为池内可用的已认证连接（失败时连接对象可能处于断开状态）。 */
    virtual bool restoreSessionAfterRawRelay() = 0;
};

using BackendConnectionPtr = std::shared_ptr<BackendConnection>;
using ConnectionPtr = BackendConnectionPtr;

}  // namespace dbproxy

#endif  // DB_PROXY_BACKEND_CONNECTION_H
