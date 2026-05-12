/**
 * @file test_pooled_session_relay.cpp
 * @brief 验证透明 TCP 中继与池化路径编排（不依赖真实 MySQL/PG）
 */

#include "core/logger.h"
#include "pool/backend_connection.h"
#include "protocol/transparent_session_relay.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int fail_count = 0;

void checkFail(const char* expr, const char* file, int line) {
    std::cerr << "CHECK failed: " << expr << " at " << file << ":" << line << "\n";
    ++fail_count;
}

#define CHECK(cond) \
    do { \
        if (!(cond)) checkFail(#cond, __FILE__, __LINE__); \
    } while (0)

bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

uint16_t bindListenRandomPort(int listen_fd) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool tcpConnectLoopback(uint16_t port, int& out_fd, std::string& err) {
    out_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (out_fd < 0) {
        err = "socket";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(out_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "connect";
        ::close(out_fd);
        out_fd = -1;
        return false;
    }
    return true;
}

/**
 * 本机 TCP echo：每个 accept 的连接在独立线程中 recv/send 回显直到对端关闭。
 */
class TcpEchoServer {
public:
    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        port_ = bindListenRandomPort(listen_fd_);
        CHECK(port_ != 0);
        CHECK(::listen(listen_fd_, 8) == 0);

        th_ = std::thread([this] {
            while (!stop_.load()) {
                pollfd pfd{};
                pfd.fd = listen_fd_;
                pfd.events = POLLIN;
                const int pr = ::poll(&pfd, 1, 100);
                if (pr <= 0) {
                    continue;
                }
                if (!(pfd.revents & POLLIN)) {
                    continue;
                }
                sockaddr_in cli{};
                socklen_t clen = sizeof(cli);
                const int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen);
                if (cfd < 0) {
                    continue;
                }
                std::thread([cfd] {
                    std::vector<char> buf(64 * 1024);
                    for (;;) {
                        const ssize_t n = ::recv(cfd, buf.data(), buf.size(), 0);
                        if (n <= 0) {
                            break;
                        }
                        size_t sent = 0;
                        while (sent < static_cast<size_t>(n)) {
                            const ssize_t w =
                                ::send(cfd, buf.data() + sent, static_cast<size_t>(n) - sent, 0);
                            if (w <= 0) {
                                goto done;
                            }
                            sent += static_cast<size_t>(w);
                        }
                    }
                done:
                    ::shutdown(cfd, SHUT_RDWR);
                    ::close(cfd);
                }).detach();
            }
            ::close(listen_fd_);
            listen_fd_ = -1;
        });
    }

    void stop() {
        stop_.store(true);
        if (th_.joinable()) {
            th_.join();
        }
    }

    uint16_t port() const { return port_; }

private:
    int listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::thread th_;
};

/**
 * 模拟池化后端：enterRawWireRelayMode 仅建立到 echo 端口的裸 TCP；restore 关闭裸连接。
 */
class EchoRawBackendStub : public dbproxy::BackendConnection {
public:
    explicit EchoRawBackendStub(uint16_t echo_port) : echo_port_(echo_port) {}

    bool connect() override { return true; }

    void close() override {
        if (raw_fd_ >= 0) {
            ::shutdown(raw_fd_, SHUT_RDWR);
            ::close(raw_fd_);
            raw_fd_ = -1;
        }
        st_ = State::CLOSED;
    }

    bool isConnected() const override { return raw_fd_ >= 0; }

    bool ping() override { return raw_fd_ >= 0; }

    bool execute(const std::string&) override { return false; }

    bool sendRaw(const char*, size_t) override { return false; }

    ssize_t recvRaw(char*, size_t, std::chrono::milliseconds) override { return -1; }

    const std::vector<Column>& resultColumns() const override { return cols_; }

    const std::vector<std::vector<std::string>>& resultRows() const override { return rows_; }

    int affectedRows() const override { return 0; }

    const std::string& lastError() const override { return last_error_; }

    void clearResult() override {}

    State state() const override { return st_; }

    void setState(State s) override { st_ = s; }

    int fd() const override { return raw_fd_; }

    uint64_t id() const override { return 1; }

    const std::string& remoteHost() const override { return host_; }

    uint16_t remotePort() const override { return echo_port_; }

    dbproxy::BackendProtocol protocol() const override { return dbproxy::BackendProtocol::MySQL; }

    std::chrono::steady_clock::time_point lastActiveTime() const override { return t0_; }

    void updateActiveTime() override { t0_ = std::chrono::steady_clock::now(); }

    int refCount() const override { return 0; }

    void addRef() override {}

    void releaseRef() override {}

    bool enterRawWireRelayMode() override {
        std::string err;
        if (!tcpConnectLoopback(echo_port_, raw_fd_, err)) {
            last_error_ = err;
            return false;
        }
        int flags = fcntl(raw_fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(raw_fd_, F_SETFL, flags & ~O_NONBLOCK) < 0) {
            last_error_ = "fcntl";
            ::close(raw_fd_);
            raw_fd_ = -1;
            return false;
        }
        st_ = State::IN_USE;
        return true;
    }

    bool restoreSessionAfterRawRelay() override {
        if (raw_fd_ >= 0) {
            ::shutdown(raw_fd_, SHUT_RDWR);
            ::close(raw_fd_);
            raw_fd_ = -1;
        }
        restored_ = true;
        st_ = State::IDLE;
        return true;
    }

    bool restored() const { return restored_; }

private:
    uint16_t echo_port_;
    int raw_fd_{-1};
    State st_{State::IDLE};
    std::string last_error_;
    std::string host_{"127.0.0.1"};
    std::vector<Column> cols_;
    std::vector<std::vector<std::string>> rows_;
    std::chrono::steady_clock::time_point t0_{std::chrono::steady_clock::now()};
    bool restored_{false};
};

void test_transparent_relay_roundtrip() {
    TcpEchoServer echo;
    echo.start();
    CHECK(echo.port() != 0);

    int sv[2]{-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(setNonBlocking(sv[1]));

    std::thread relay([&] { dbproxy::runTransparentTcpSessionRelay(sv[0], "127.0.0.1", echo.port()); });

    const char payload[] = "relay-check-42";
    CHECK(::send(sv[1], payload, sizeof(payload) - 1, 0) == static_cast<ssize_t>(sizeof(payload) - 1));

    char inbuf[128]{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    ssize_t got = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = sv[1];
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            got = ::recv(sv[1], inbuf, sizeof(inbuf) - 1, 0);
            if (got > 0) {
                break;
            }
        }
    }
    CHECK(got == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::memcmp(inbuf, payload, static_cast<size_t>(got)) == 0);

    // 让中继在 client_fd 上读到 EOF，从而退出 poll 循环
    ::shutdown(sv[1], SHUT_RDWR);
    relay.join();
    ::close(sv[1]);

    echo.stop();
}

void test_pooled_relay_roundtrip() {
    TcpEchoServer echo;
    echo.start();
    CHECK(echo.port() != 0);

    int sv[2]{-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(setNonBlocking(sv[1]));

    auto stub = std::make_shared<EchoRawBackendStub>(echo.port());

    std::thread relay([&] { dbproxy::runPooledTransparentSessionRelay(sv[0], stub); });

    const char payload[] = "pooled-relay-99";
    CHECK(::send(sv[1], payload, sizeof(payload) - 1, 0) == static_cast<ssize_t>(sizeof(payload) - 1));

    char inbuf[128]{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    ssize_t got = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = sv[1];
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            got = ::recv(sv[1], inbuf, sizeof(inbuf) - 1, 0);
            if (got > 0) {
                break;
            }
        }
    }
    CHECK(got == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::memcmp(inbuf, payload, static_cast<size_t>(got)) == 0);

    ::shutdown(sv[1], SHUT_RDWR);
    relay.join();
    CHECK(stub->restored());

    ::close(sv[1]);

    echo.stop();
}

}  // namespace

int main() {
    dbproxy::Logger::instance().init("", dbproxy::LogLevel::WARN);

    test_transparent_relay_roundtrip();
    test_pooled_relay_roundtrip();

    if (fail_count != 0) {
        std::cerr << fail_count << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_pooled_session_relay: OK\n";
    return 0;
}
