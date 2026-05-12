/**
 * @file test_monitor_integration.cpp
 * @brief 监控模块集成测试：Prometheus /metrics、Statistics 会话统计、[monitor] 配置解析
 */

#include "core/config.h"
#include "core/logger.h"
#include "monitor/metrics.h"
#include "monitor/metrics_http_server.h"
#include "monitor/statistics.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

uint16_t pickFreeListenPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

std::string httpGet(const std::string& host, uint16_t port, const std::string& request_line) {
    const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) {
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "127.0.0.1" || host == "localhost") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(cfd);
        return {};
    }
    if (::connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(cfd);
        return {};
    }
    std::ostringstream req;
    req << request_line << "\r\nHost: test\r\nConnection: close\r\n\r\n";
    const std::string rq = req.str();
    if (::send(cfd, rq.data(), rq.size(), 0) != static_cast<ssize_t>(rq.size())) {
        ::close(cfd);
        return {};
    }
    std::string out;
    char buf[4096];
    for (int i = 0; i < 64; ++i) {
        pollfd pfd{};
        pfd.fd = cfd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 500) <= 0) {
            break;
        }
        const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    ::shutdown(cfd, SHUT_RDWR);
    ::close(cfd);
    return out;
}

void test_metrics_http_get_metrics() {
    dbproxy::Metrics::instance().reset();
    dbproxy::Metrics::instance().incrementCounter("integration_test_counter", 7);

    const uint16_t port = pickFreeListenPort();
    CHECK(port != 0);

    dbproxy::MetricsHttpServer srv;
    std::string err;
    CHECK(srv.start("127.0.0.1", port, err));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string resp =
        httpGet("127.0.0.1", port, "GET /metrics HTTP/1.1");
    CHECK(resp.find("HTTP/1.1 200") != std::string::npos);
    CHECK(resp.find("integration_test_counter") != std::string::npos);

    const std::string resp404 = httpGet("127.0.0.1", port, "GET /unknown HTTP/1.1");
    CHECK(resp404.find("HTTP/1.1 404") != std::string::npos);
}

void test_statistics_record_relay_session_end() {
    dbproxy::Statistics::instance().reset();

    dbproxy::Statistics::instance().recordRelaySessionEnd("10.0.0.1", 10, 100);
    dbproxy::Statistics::instance().recordRelaySessionEnd("10.0.0.2", 200, 100);

    auto stats = dbproxy::Statistics::instance().getGlobalStats();
    CHECK(stats.total_connections >= 2);
    CHECK(stats.total_queries >= 2);
    CHECK(stats.slow_queries >= 1);

    auto clients = dbproxy::Statistics::instance().getTopClients(10);
    CHECK(clients.size() >= 2);
}

void test_load_config_monitor_keys() {
    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path() / "dbproxy_monitor_ini_test.conf";
    {
        std::ofstream out(tmp);
        CHECK(out.is_open());
        out << "[monitor]\n";
        out << "enable = false\n";
        out << "metrics_interval_ms = 2345\n";
        out << "metrics_host = 0.0.0.0\n";
        out << "metrics_port = 19998\n";
        out << "slow_query_threshold_ms = 77\n";
        out << "enable_query_logging = true\n";
    }
    const dbproxy::Config cfg = dbproxy::loadConfig(tmp.string());
    CHECK(cfg.monitoring.enable == false);
    CHECK(cfg.monitoring.metrics_interval_ms == 2345);
    CHECK(cfg.monitoring.metrics_host == "0.0.0.0");
    CHECK(cfg.monitoring.metrics_port == 19998);
    CHECK(cfg.monitoring.slow_query_threshold_ms == 77);
    CHECK(cfg.monitoring.enable_query_logging == true);
    std::error_code ec;
    fs::remove(tmp, ec);
}

}  // namespace

int main() {
    dbproxy::Logger::instance().init("", dbproxy::LogLevel::WARN);

    test_metrics_http_get_metrics();
    test_statistics_record_relay_session_end();
    test_load_config_monitor_keys();

    if (fail_count != 0) {
        std::cerr << fail_count << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_monitor_integration: OK\n";
    return 0;
}
