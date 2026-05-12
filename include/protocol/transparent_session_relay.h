#ifndef DB_PROXY_TRANSPARENT_SESSION_RELAY_H
#define DB_PROXY_TRANSPARENT_SESSION_RELAY_H

#include "pool/backend_connection.h"

#include <cstdint>
#include <string>

namespace dbproxy {

/**
 * 客户端与数据库后端之间的透明双向 TCP 转发（适用于 MySQL / PostgreSQL 等基于 TCP 的线协议）。
 * 调用方须已将 client_fd 从 EpollServer 摘除；函数返回时关闭两端套接字。
 */
void runTransparentTcpSessionRelay(int client_fd, const std::string& host, uint16_t port);

/**
 * 池化路径：对 backend 调用 enterRawWireRelayMode() 后，在 client_fd 与 backend->fd() 间双向透传；
 * 中继结束时不关闭后端套接字（仅 shutdown），由 restoreSessionAfterRawRelay() 关闭并恢复池内会话。
 * 始终关闭 client_fd。
 */
void runPooledTransparentSessionRelay(int client_fd, BackendConnectionPtr backend);

}  // namespace dbproxy

#endif
