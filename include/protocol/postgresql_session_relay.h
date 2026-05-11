#ifndef DB_PROXY_POSTGRESQL_SESSION_RELAY_H
#define DB_PROXY_POSTGRESQL_SESSION_RELAY_H

#include "protocol/transparent_session_relay.h"

namespace dbproxy {

inline void runPostgresqlSessionRelay(int client_fd, const std::string& host, uint16_t port) {
    runTransparentTcpSessionRelay(client_fd, host, port);
}

}  // namespace dbproxy

#endif
