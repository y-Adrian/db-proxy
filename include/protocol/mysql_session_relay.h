#ifndef DB_PROXY_MYSQL_SESSION_RELAY_H
#define DB_PROXY_MYSQL_SESSION_RELAY_H

#include <cstdint>
#include <string>

namespace dbproxy {

/**
 * 在单条 TCP 连接上做客户端与 MySQL 后端之间的透明双向字节转发（含握手与认证）。
 * 调用方须已将该 client_fd 从 EpollServer 中摘除；本函数结束时将关闭 client_fd 与后端套接字。
 */
void runMysqlSessionRelay(int client_fd, const std::string& host, uint16_t port);

}  // namespace dbproxy

#endif
