/**
 * @file main.cpp
 * @brief 数据库代理中间件 - 主程序入口
 *
 * 修复清单（相对于原始版本）：
 *   Fix-1  MySQL 透明会话代理：accept 后工作线程对客户端与后端做双向字节转发（含握手/认证）
 *   Fix-2  使用 INI 配置文件（proxy.conf），不再硬编码参数
 *   Fix-3  后台线程改用 std::jthread + std::stop_token，保证优雅退出
 *   Fix-4  新增 RequestWorkerPool，将阻塞式后端 I/O 移出 epoll 事件循环
 */

#include "network/epoll_server.h"
#include "network/tcp_connection.h"
#include "pool/pool_manager.h"
#include "monitor/metrics.h"
#include "monitor/statistics.h"
#include "monitor/performance_analyzer.h"
#include "core/logger.h"
#include "core/config.h"
#include "protocol/mysql_session_relay.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

using namespace dbproxy;

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
    std::string config_path = "proxy.conf";
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
        LOG_ERROR("No database configured – check proxy.conf [database] section");
        return 1;
    }

    const auto& primary_db = config.databases[0];
    const std::string pool_name = "default";

    bool pool_ok = PoolManager::instance().addPool(
        pool_name,
        primary_db.host,
        primary_db.port,
        primary_db.username,
        primary_db.password,
        primary_db.database,
        static_cast<size_t>(config.pool.min_connections),
        static_cast<size_t>(config.pool.max_connections),
        BackendProtocol::MySQL,
        std::chrono::milliseconds(config.pool.max_idle_time_ms),
        std::chrono::milliseconds(config.pool.connection_timeout_ms)
    );
    if (!pool_ok) {
        LOG_ERROR("Failed to create connection pool for {}:{}/{}",
                  primary_db.host, primary_db.port, primary_db.database);
        return 1;
    }
    LOG_INFO("Pool '{}' → {}:{}/{} (min={} max={})",
             pool_name, primary_db.host, primary_db.port, primary_db.database,
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

    g_server->setConnectionCallback(
        [&workers, host = std::string(primary_db.host), port = primary_db.port](auto conn) {
            LOG_DEBUG("New connection: {}:{}", conn->remoteIp(), conn->remotePort());
            METRICS_INC("connections_total");
            METRICS_GAUGE_SET("active_connections",
                Metrics::instance().getGauge("active_connections") + 1);

            if (!g_server) {
                return;
            }
            auto taken = g_server->takeConnection(conn->fd());
            if (!taken) {
                LOG_ERROR("Internal error: takeConnection failed for fd {}", conn->fd());
                METRICS_GAUGE_SET("active_connections",
                    Metrics::instance().getGauge("active_connections") - 1);
                return;
            }
            const int cfd = taken->releaseFd();
            workers->post([cfd, host, port]() {
                runMysqlSessionRelay(cfd, host, port);
                METRICS_GAUGE_SET("active_connections",
                    Metrics::instance().getGauge("active_connections") - 1);
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
    std::jthread health_thread([health_interval](std::stop_token stoken) {
        LOG_INFO("Health-check thread started");
        while (!stoken.stop_requested()) {
            // 将长睡眠拆成短片段，使 stop_token 能快速响应
            for (int i = 0; i < 100 && !stoken.stop_requested(); ++i) {
                std::this_thread::sleep_for(health_interval / 100);
            }
            if (!stoken.stop_requested()) {
                PoolManager::instance().healthCheckAll();
                PerformanceAnalyzer::instance().collectSnapshot();
            }
        }
        LOG_INFO("Health-check thread stopped");
    });

    std::jthread stats_thread([](std::stop_token stoken) {
        LOG_INFO("Stats thread started");
        while (!stoken.stop_requested()) {
            for (int i = 0; i < 50 && !stoken.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stoken.stop_requested()) break;

            auto stats = Statistics::instance().getGlobalStats();
            auto pools = PoolManager::instance().getAllStats();

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

    // ---- 启动主循环 ----
    LOG_INFO("Server listening on {}:{}", config.server.host, config.server.port);
    g_server->start();   // blocks until stop() is called

    // ---- 清理（jthread 析构自动停止并 join 后台线程）----
    LOG_INFO("Stopping worker pool and connection pools...");
    workers.reset();
    PoolManager::instance().shutdownAll();
    LOG_INFO("=== Database Proxy Stopped ===");
    return 0;
}
