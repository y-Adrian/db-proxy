#include "protocol/transparent_session_relay.h"
#include "core/logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace dbproxy {

namespace detail {

bool setBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

int connectTcpBackend(const std::string& host, uint16_t port, std::string& err) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "socket";
        return -1;
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (he == nullptr) {
            err = "resolve " + host;
            ::close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], static_cast<size_t>(he->h_length));
    }

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("connect: ") + strerror(errno);
        ::close(fd);
        return -1;
    }

    return fd;
}

}  // namespace detail

/**
 * @param close_backend_socket_when_done 为 true 时关闭 backend_fd（独立 connect 场景）；
 *        为 false 时仅 shutdown(backend_fd)，由 BackendConnection::restoreSessionAfterRawRelay() 负责 close。
 */
static void tcpRelayBidirectionalBlocking(int client_fd, int backend_fd, bool close_backend_socket_when_done) {
    if (!detail::setBlocking(client_fd) || !detail::setBlocking(backend_fd)) {
        LOG_ERROR("Transparent session relay: fcntl failed");
        if (close_backend_socket_when_done) {
            ::close(backend_fd);
        }
        ::close(client_fd);
        return;
    }

    std::vector<char> buf(256 * 1024);
    struct pollfd pfds[2];
    pfds[0].fd = client_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = backend_fd;
    pfds[1].events = POLLIN;

    for (;;) {
        const int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & (POLLERR | POLLNVAL)) {
            break;
        }
        if (pfds[1].revents & (POLLERR | POLLNVAL)) {
            break;
        }

        if (pfds[0].revents & POLLHUP) {
            break;
        }
        if (pfds[1].revents & POLLHUP) {
            if (!(pfds[0].revents & POLLIN)) {
                break;
            }
        }

        if (pfds[0].revents & POLLIN) {
            const ssize_t n = ::recv(client_fd, buf.data(), buf.size(), 0);
            if (n <= 0) {
                break;
            }
            if (!detail::sendAll(backend_fd, buf.data(), static_cast<size_t>(n))) {
                break;
            }
        }

        if (pfds[1].revents & POLLIN) {
            const ssize_t n = ::recv(backend_fd, buf.data(), buf.size(), 0);
            if (n <= 0) {
                break;
            }
            if (!detail::sendAll(client_fd, buf.data(), static_cast<size_t>(n))) {
                break;
            }
        }
    }

    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
    ::shutdown(backend_fd, SHUT_RDWR);
    if (close_backend_socket_when_done) {
        ::close(backend_fd);
    }
    LOG_DEBUG("Transparent TCP session relay ended");
}

void runTransparentTcpSessionRelay(int client_fd, const std::string& host, uint16_t port) {
    std::string err;
    const int backend_fd = detail::connectTcpBackend(host, port, err);
    if (backend_fd < 0) {
        LOG_ERROR("Transparent session relay: cannot connect backend {}:{} — {}", host, port, err);
        ::close(client_fd);
        return;
    }

    tcpRelayBidirectionalBlocking(client_fd, backend_fd, true);
}

void runPooledTransparentSessionRelay(int client_fd, BackendConnectionPtr backend) {
    if (!backend) {
        LOG_ERROR("Pooled transparent relay: null backend connection");
        ::close(client_fd);
        return;
    }
    if (!backend->enterRawWireRelayMode()) {
        LOG_ERROR("Pooled transparent relay: enterRawWireRelayMode failed: {}", backend->lastError());
        ::close(client_fd);
        return;
    }
    const int bfd = backend->fd();
    tcpRelayBidirectionalBlocking(client_fd, bfd, false);
    if (!backend->restoreSessionAfterRawRelay()) {
        LOG_WARN("Pooled transparent relay: restoreSessionAfterRawRelay failed: {}", backend->lastError());
    }
}

}  // namespace dbproxy
