/**
 * @file main.cpp
 * @brief 数据库代理中间件 `db-proxy` — 主程序入口
 *
 * ## 职责概览
 * - **监听**：`EpollServer` 在 `[server]` 配置的地址/端口上接受客户端 TCP 连接（MySQL 或 PostgreSQL 线协议，
 *   由首个 `[database.*]` 的 `protocol` 决定；`mysql` / `postgresql`（或 `postgres` / `pg`））。
 * - **池化后端**：`PoolManager` 为每个数据库节注册 `ConnectionPool`；工作线程从池借出**已握手**的后端连接，
 *   与客户端会话做**透明双向字节透传**（`TransparentSessionRelay`），会话结束后 `restoreSessionAfterRawRelay`
 *   再归还池。
 * - **配置**：INI（`-c` / `--config` / `--config=`），见 `loadConfig`；示例按后端分文件放在 `conf/`（`conf/proxy.mysql.conf`、`conf/proxy.postgresql.conf`）。未传 `-c` 时依次尝试 `conf/` 与 `../conf/` 下的 `proxy.mysql.conf` 以兼容 cwd 为仓库根或 `build/`。
 * - **监控**：`[monitor]` 控制统计日志、会话级 Metrics/Statistics、`PerformanceAnalyzer` 快照；`metrics_port>0`
 *   时可选 `MetricsHttpServer` 暴露 `GET /metrics`（Prometheus 文本）。
 *
 * ## 后端协议与依赖
 * - **MySQL**：池内握手由 `Connection` 完成；支持 `mysql_native_password`（内置 SHA-1）与
 *   `caching_sha2_password`（内置 SHA-256 SCRAMBLE；若服务端要求 RSA「完整认证」，需 **OpenSSL** 构建，
 *   见 `DBPROXY_HAVE_OPENSSL` 与 `connection.cpp`）。
 * - **PostgreSQL**：`protocol` 为 PG 时使用 `PostgreSQLConnection`；认证实现 trust / 明文 / MD5 /
 *   **SCRAM-SHA-256**（依赖 **OpenSSL**）。若 CMake 未找到 OpenSSL，`DBPROXY_ENABLE_POSTGRES` 会被关闭，
 *   主程序仍可对 MySQL 运行，但无法注册 PG 池。
 *
 * ## 线程模型（Fix-3 / Fix-4）
 * - 统计与健康检查等后台任务：`std::jthread` + `std::stop_token`。
 * - 每会话后端 I/O：`RequestWorkerPool`（固定大小 `std::thread` 池），避免阻塞 epoll 线程。
 *
 * ## 历史修复标签（相对早期单文件版本）
 * - **Fix-1** 池化 + 透明 TCP 会话代理（MySQL / PostgreSQL）。
 * - **Fix-2** INI 配置，取代硬编码。
 * - **Fix-3** 后台线程改为 `jthread` + `stop_token`；会话仍用线程池。
 * - **Fix-5** `[monitor]` 接入：统计、会话 Metrics、可选 `/metrics`。
 */
#include "network/epoll_server.h"
#include "network/tcp_connection.h"
#include "pool/pool_manager.h"
#include "monitor/metrics.h"
#include "monitor/metrics_http_server.h"
#include "monitor/statistics.h"
#include "monitor/performance_analyzer.h"
#include "core/logger.h"
#include "core/config.h"
#include "pool/backend_connection.h"
#include "protocol/transparent_session_relay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>

using namespace dbproxy;

namespace {

bool isPostgresWireProtocol(const std::string& protocol) {
    std::string p = protocol;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return p == "postgresql" || p == "postgres" || p == "pg";
}

/** 默认 MySQL 示例配置：优先仓库根下的 conf/，其次 build/ 工作目录下的 ../conf/。 */
std::string defaultMysqlConfigPath() {
    constexpr const char* kCandidates[] = {"conf/proxy.mysql.conf", "../conf/proxy.mysql.conf"};
    for (const char* p : kCandidates) {
        std::ifstream f(p);
        if (f.good()) {
            return p;
        }
    }
    return kCandidates[0];
}

}  // namespace

// ============================================================================
// Fix-3: 全局停止标志（jthread 通过 stop_token 感知；信号处理也需要此标志）
// ============================================================================
static std::atomic<bool> g_running{true};
static std::unique_ptr<EpollServer> g_server;

void signalHandler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running.store(false);
    if (g_server) g_server->stop();
}

// ============================================================================
// Fix-4: 简单的固定大小工作线程池
//        将每个客户端请求的后端 I/O 派发到独立线程，
//        避免在 epoll 事件循环线程中做阻塞操作。
// ============================================================================
class RequestWorkerPool {
public:
    explicit RequestWorkerPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~RequestWorkerPool() {
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    // 非阻塞投递任务
    void post(std::function<void()> task) {
        {
            std::lock_guard lock(mu_);
            if (stop_) return;
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    size_t pendingTasks() const {
        std::lock_guard lock(mu_);
        return queue_.size();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            try {
                task();
            } catch (const std::exception& e) {
                LOG_ERROR("Worker task threw: {}", e.what());
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
};

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    // ---- 配置文件路径（Fix-2） ----
    std::string config_path = defaultMysqlConfigPath();
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        }
    }

    // ---- 加载配置 ----
    Config config = loadConfig(config_path);

    // ---- 初始化日志 ----
    LogLevel log_level = LogLevel::INFO;
    if (config.log_level == "DEBUG") log_level = LogLevel::DEBUG;
    else if (config.log_level == "WARN")  log_level = LogLevel::WARN;
    else if (config.log_level == "ERROR") log_level = LogLevel::ERROR;

    Logger::instance().init(config.log_file, log_level);
    LOG_INFO("=== Database Proxy Starting ===");
    LOG_INFO("Config: {}", config_path);

    // ---- 注册连接池 ----
    if (config.databases.empty()) {
        LOG_ERROR("No database configured – check conf/proxy.*.conf [database] section");
        return 1;
    }

    const auto& primary_db = config.databases[0];
    const std::string pool_name = "default";
    const auto pool_acquire_timeout =
        std::chrono::milliseconds(config.pool.connection_timeout_ms);
    const bool backend_is_pg = isPostgresWireProtocol(primary_db.protocol);
    const BackendProtocol pool_proto =
        backend_is_pg ? BackendProtocol::PostgreSQL : BackendProtocol::MySQL;

    std::string pool_user = primary_db.username;
    if (backend_is_pg && pool_user.empty()) {
        if (const char* u = std::getenv("USER")) {
            pool_user = u;
        }
        if (pool_user.empty()) {
            if (const char* u = std::getenv("USERNAME")) {
                pool_user = u;
            }
        }
        if (!pool_user.empty()) {
            LOG_INFO("PostgreSQL pool user empty in config; using USER={}", pool_user);
        }
    }
    if (backend_is_pg && pool_user.empty()) {
        LOG_ERROR("PostgreSQL requires [database] user or USER/USERNAME environment variable");
        return 1;
    }

    bool pool_ok = PoolManager::instance().addPool(
        pool_name,
        primary_db.host,
        primary_db.port,
        pool_user,
        primary_db.password,
        primary_db.database,
        static_cast<size_t>(config.pool.min_connections),
        static_cast<size_t>(config.pool.max_connections),
        pool_proto,
        std::chrono::milliseconds(config.pool.max_idle_time_ms),
        std::chrono::milliseconds(config.pool.connection_timeout_ms)
    );
    if (!pool_ok) {
        LOG_ERROR("Failed to create connection pool for {}:{}/{}",
                  primary_db.host, primary_db.port, primary_db.database);
        return 1;
    }
    LOG_INFO("Backend wire protocol: {} (pooled backend + transparent wire relay per client)",
             backend_is_pg ? "PostgreSQL" : "MySQL");
    LOG_INFO("Pool '{}' user={} → {}:{}/{} (min={} max={})",
             pool_name, pool_user, primary_db.host, primary_db.port, primary_db.database,
             config.pool.min_connections, config.pool.max_connections);

    // ---- Fix-4: 工作线程池 ----
    auto workers = std::make_unique<RequestWorkerPool>(
        static_cast<size_t>(config.server.worker_threads));
    LOG_INFO("Worker threads: {}", config.server.worker_threads);

    // ---- 创建并配置服务器 ----
    g_server = std::make_unique<EpollServer>();

    if (!g_server->listen(config.server.host, config.server.port)) {
        LOG_ERROR("Failed to start server on {}:{}", config.server.host, config.server.port);
        return 1;
    }

    const MonitoringConfig mon = config.monitoring;

    g_server->setConnectionCallback(
        [&workers, pool_name, pool_acquire_timeout, mon](auto conn) {
            const std::string client_ip = conn->remoteIp();
            LOG_DEBUG("New connection: {}:{}", client_ip, conn->remotePort());
            METRICS_INC("connections_total");
            Metrics::instance().incrementGauge("active_connections");

            if (!g_server) {
                Metrics::instance().decrementGauge("active_connections");
                return;
            }
            auto taken = g_server->takeConnection(conn->fd());
            if (!taken) {
                LOG_ERROR("Internal error: takeConnection failed for fd {}", conn->fd());
                Metrics::instance().decrementGauge("active_connections");
                return;
            }
            const int cfd = taken->releaseFd();
            workers->post([cfd, pool_name, pool_acquire_timeout, client_ip, mon]() {
                const auto t0 = std::chrono::steady_clock::now();
                auto backend = PoolManager::instance().getConnection(pool_name, pool_acquire_timeout);
                if (!backend) {
                    LOG_ERROR("Pooled backend acquire failed (pool exhausted or timeout)");
                    ::close(cfd);
                    Metrics::instance().decrementGauge("active_connections");
                    return;
                }
                runPooledTransparentSessionRelay(cfd, backend);
                PoolManager::instance().returnConnection(pool_name, backend);
                const auto t1 = std::chrono::steady_clock::now();
                const auto duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    t1 - t0).count());

                if (mon.enable) {
                    const uint64_t slow_th =
                        static_cast<uint64_t>(std::max(0, mon.slow_query_threshold_ms));
                    Statistics::instance().recordRelaySessionEnd(client_ip, duration_ms, slow_th);
                    METRICS_LATENCY("relay_session_duration_ms", std::chrono::milliseconds(
                        static_cast<int64_t>(duration_ms)));
                    METRICS_INC("relay_sessions_total");
                    if (mon.enable_query_logging) {
                        LOG_INFO("relay session end client={} duration_ms={}", client_ip, duration_ms);
                    }
                    if (slow_th > 0 && duration_ms >= slow_th) {
                        LOG_WARN("slow relay session client={} duration_ms={} (threshold_ms={})",
                                 client_ip, duration_ms, mon.slow_query_threshold_ms);
                    }
                }

                Metrics::instance().decrementGauge("active_connections");
            });
        });

    // 连接已交给工作线程，不再由 Reactor 按消息回调处理
    g_server->setMessageCallback([](auto, const char*, size_t) {});

    g_server->setCloseCallback([](auto conn) {
        LOG_DEBUG("Connection closed (unexpected path): {}", conn->remoteIp());
    });

    // ---- 信号处理 ----
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---- Fix-3: 后台线程改用 jthread（析构时自动 request_stop + join）----
    auto health_interval = std::chrono::milliseconds(config.pool.health_check_interval_ms);
    std::jthread health_thread([health_interval, mon_en = config.monitoring.enable](std::stop_token stoken) {
        LOG_INFO("Health-check thread started");
        while (!stoken.stop_requested()) {
            // 将长睡眠拆成短片段，使 stop_token 能快速响应
            for (int i = 0; i < 100 && !stoken.stop_requested(); ++i) {
                std::this_thread::sleep_for(health_interval / 100);
            }
            if (!stoken.stop_requested()) {
                PoolManager::instance().healthCheckAll();
                if (mon_en) {
                    PerformanceAnalyzer::instance().collectSnapshot();
                }
            }
        }
        LOG_INFO("Health-check thread stopped");
    });

    const int stats_interval_ms = std::max(100, config.monitoring.metrics_interval_ms);
    std::jthread stats_thread([stats_interval_ms, mon_en = config.monitoring.enable](std::stop_token stoken) {
        if (!mon_en) {
            LOG_INFO("Periodic stats disabled ([monitor] enable=false)");
            return;
        }
        LOG_INFO("Stats thread started (interval={} ms)", stats_interval_ms);
        const int slices = 50;
        const int slice_ms = std::max(1, stats_interval_ms / slices);
        while (!stoken.stop_requested()) {
            for (int i = 0; i < slices && !stoken.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            }
            if (stoken.stop_requested()) {
                break;
            }

            Statistics::instance().setActiveConnections(
                static_cast<int>(Metrics::instance().getGauge("active_connections")));

            auto stats = Statistics::instance().getGlobalStats();
            auto pools = PoolManager::instance().getAllStats();
            for (const auto& p : pools) {
                METRICS_GAUGE_SET("pool_connections_total", static_cast<double>(p.total));
                METRICS_GAUGE_SET("pool_connections_idle", static_cast<double>(p.idle));
                METRICS_GAUGE_SET("pool_connections_busy", static_cast<double>(p.busy));
            }
            METRICS_GAUGE_SET("qps", stats.current_qps);

            LOG_INFO("=== Stats ===");
            LOG_INFO("QPS: {} | Active connections: {}",
                     stats.current_qps, stats.active_connections);
            for (const auto& p : pools) {
                LOG_INFO("Pool [{}]: total={} idle={} busy={}",
                         p.name, p.total, p.idle, p.busy);
            }
        }
        LOG_INFO("Stats thread stopped");
    });

    std::unique_ptr<MetricsHttpServer> metrics_http;
    if (config.monitoring.enable && config.monitoring.metrics_port > 0) {
        metrics_http = std::make_unique<MetricsHttpServer>();
        std::string mh_err;
        if (!metrics_http->start(config.monitoring.metrics_host, config.monitoring.metrics_port, mh_err)) {
            LOG_WARN("Prometheus /metrics not started: {}", mh_err);
            metrics_http.reset();
        } else {
            LOG_INFO("Prometheus metrics: http://{}:{}/metrics",
                     config.monitoring.metrics_host,
                     static_cast<int>(config.monitoring.metrics_port));
        }
    }

    // ---- 启动主循环 ----
    LOG_INFO("Server listening on {}:{}", config.server.host, config.server.port);
    g_server->start();   // blocks until stop() is called

    // ---- 清理（jthread 析构自动停止并 join 后台线程）----
    LOG_INFO("Stopping worker pool and connection pools...");
    metrics_http.reset();
    workers.reset();
    PoolManager::instance().shutdownAll();
    LOG_INFO("=== Database Proxy Stopped ===");
    return 0;
}
