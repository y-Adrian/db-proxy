#ifndef DB_PROXY_METRICS_HTTP_SERVER_H
#define DB_PROXY_METRICS_HTTP_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace dbproxy {

/**
 * 极简阻塞式 HTTP 服务：仅响应 GET /metrics（Prometheus 文本）。
 * 在独立 std::jthread 中 accept；析构时 request_stop 并 shutdown 监听 fd。
 */
class MetricsHttpServer {
public:
    MetricsHttpServer() = default;
    ~MetricsHttpServer();

    MetricsHttpServer(const MetricsHttpServer&) = delete;
    MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;

    /** 启动失败返回 false，err_out 填原因 */
    bool start(const std::string& bind_host, uint16_t port, std::string& err_out);

private:
    int listen_fd_{-1};
    std::unique_ptr<std::jthread> thread_;
};

}  // namespace dbproxy

#endif
