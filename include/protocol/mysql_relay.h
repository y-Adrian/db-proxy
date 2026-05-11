#ifndef DB_PROXY_MYSQL_RELAY_H
#define DB_PROXY_MYSQL_RELAY_H

#include <chrono>

namespace dbproxy {

class BackendConnection;
class TcpConnection;

/**
 * 将一次 COM_* 命令产生的服务端响应按 MySQL 包边界完整转发到客户端。
 * 覆盖典型查询结果集（列元数据 + EOF + 行 + EOF）、OK、ERR；不处理 >16MB 分包与
 * LOCAL INFILE（后者在转发首包后回退为短空闲超时读，避免挂死）。
 */
bool relayMysqlServerResponse(
    BackendConnection& backend,
    TcpConnection& client,
    std::chrono::milliseconds first_packet_timeout = std::chrono::seconds(5),
    std::chrono::milliseconds packet_io_timeout = std::chrono::seconds(120));

}  // namespace dbproxy

#endif  // DB_PROXY_MYSQL_RELAY_H
