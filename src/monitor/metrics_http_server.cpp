#include "monitor/metrics_http_server.h"
#include "monitor/metrics.h"
#include "core/logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

namespace dbproxy {

namespace {

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    int sflags = 0;
#ifdef MSG_NOSIGNAL
    sflags |= MSG_NOSIGNAL;
#endif
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, sflags);
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

bool readHttpHeaders(int fd, std::string& out) {
    out.clear();
    out.reserve(4096);
    char buf[512];
    for (int i = 0; i < 32 && out.size() < 8192; ++i) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    return true;
}

bool isGetMetrics(const std::string& headers) {
    return headers.find("GET /metrics") != std::string::npos ||
           headers.find("get /metrics") != std::string::npos;
}

}  // namespace

MetricsHttpServer::~MetricsHttpServer() {
    if (thread_) {
        thread_->request_stop();
        thread_.reset();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool MetricsHttpServer::start(const std::string& bind_host, uint16_t port, std::string& err_out) {
    if (port == 0) {
        err_out = "metrics_port is 0";
        return false;
    }
    if (thread_) {
        err_out = "already started";
        return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        err_out = std::string("socket: ") + strerror(errno);
        return false;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string& h = bind_host.empty() ? std::string("127.0.0.1") : bind_host;
    if (h == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1) {
        err_out = "invalid metrics_host (use IPv4 literal or 0.0.0.0)";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err_out = std::string("bind: ") + strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 16) < 0) {
        err_out = std::string("listen: ") + strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    const int listen_copy = listen_fd_;
    thread_ = std::make_unique<std::jthread>([listen_copy](std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            pollfd pfd{};
            pfd.fd = listen_copy;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, 500);
            if (stoken.stop_requested()) {
                break;
            }
            if (pr <= 0) {
                continue;
            }
            if (!(pfd.revents & POLLIN)) {
                continue;
            }

            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            const int cfd = ::accept(listen_copy, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (cfd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                continue;
            }

            std::string headers;
            if (!readHttpHeaders(cfd, headers)) {
                ::shutdown(cfd, SHUT_RDWR);
                ::close(cfd);
                continue;
            }

            std::ostringstream response;
            if (isGetMetrics(headers)) {
                const std::string body = Metrics::instance().toPrometheusFormat();
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                         << "Connection: close\r\n"
                         << "Content-Length: " << body.size() << "\r\n\r\n"
                         << body;
            } else {
                const char* msg = "Not Found\r\n";
                response << "HTTP/1.1 404 Not Found\r\n"
                         << "Content-Type: text/plain\r\n"
                         << "Connection: close\r\n"
                         << "Content-Length: " << strlen(msg) << "\r\n\r\n"
                         << msg;
            }
            const std::string pkt = response.str();
            (void)sendAll(cfd, pkt);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
        }
    });

    return true;
}

}  // namespace dbproxy
