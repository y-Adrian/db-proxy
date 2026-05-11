#ifndef DB_PROXY_TRANSPARENT_SESSION_RELAY_H
#define DB_PROXY_TRANSPARENT_SESSION_RELAY_H

#include <cstdint>
#include <string>

namespace dbproxy {

/**
 * 客户端与数据库后端之间的透明双向 TCP 转发（适用于 MySQL / PostgreSQL 等基于 TCP 的线协议）。
 * 调用方须已将 client_fd 从 EpollServer 摘除；函数返回时关闭两端套接字。
 */
void runTransparentTcpSessionRelay(int client_fd, const std::string& host, uint16_t port);

}  // namespace dbproxy

#endif
