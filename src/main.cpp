/**
 * @file main.cpp
 * @brief 数据库代理中间件 - 主程序入口
 *
 * 修复清单（相对于原始版本）：
 *   Fix-1  实现客户端 → 后端 → 客户端的完整数据转发（原 TODO 占位符）
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
#include "protocol/mysql_parser.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

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
// Fix-1: 完整的双向数据转发
//
// 流程：
//   1. 从连接池取得一个后端连接
//   2. 将客户端原始字节原样发给后端（sendRaw）
//   3. 循环读取后端响应并转发回客户端（recvRaw → conn->sendInLoop）
//   4. 将后端连接归还连接池
//
// 关于响应结束判断：
//   MySQL 协议的完整响应边界需要解析包头。此处采用"短超时读取"策略：
//   若 recvRaw 在 50 ms 内无数据则认为本次响应已结束。
//   对于大结果集，循环会持续读取直至 50 ms 静默。
//   生产级实现应解析 MySQL 包边界（OK/EOF/ERR 包），此处保留
//   TODO 注释以标记改进点。
// ============================================================================
static void forwardRequest(
        std::shared_ptr<TcpConnection> conn,
        std::vector<char> data,
        const std::string& pool_name,
        std::chrono::milliseconds pool_timeout)
{
    auto start = std::chrono::steady_clock::now();

    // 1. 统计
    METRICS_INC("queries_total");

    // 2. 解析 SQL 类型（用于路由/统计，不影响转发）
    if (data.size() > 5) {
        std::string sql_str(data.data(), std::min(data.size(), size_t(256)));
        auto info = MySQLParser::parseSQL(sql_str);
        LOG_DEBUG("SQL type: {}", static_cast<int>(info.type));
    }

    // 3. 从连接池获取后端连接
    auto backend = PoolManager::instance().getConnection(pool_name, pool_timeout);
    if (!backend) {
        LOG_ERROR("Pool '{}': failed to acquire connection (timeout or pool exhausted)",
                  pool_name);
        METRICS_INC("pool_acquire_failures");
        // 向客户端发送一个标准 MySQL 错误包（Error 1040: Too many connections）
        // 格式：3字节长度 + 1字节包序号 + 0xFF + 2字节错误码 + '#' + 5字节sqlstate + 消息
        const std::string errmsg = "Too many connections";
        uint8_t sqlstate[] = {'H', 'Y', '0', '0', '0'};
        std::string err_pkt;
        uint32_t payload_len = 1 + 2 + 1 + 5 + errmsg.size();
        err_pkt.push_back(static_cast<char>(payload_len & 0xFF));
        err_pkt.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
        err_pkt.push_back(static_cast<char>((payload_len >> 16) & 0xFF));
        err_pkt.push_back(static_cast<char>(0));  // seq
        err_pkt.push_back(static_cast<char>(0xFF)); // error marker
        err_pkt.push_back(static_cast<char>(0xE8)); // errno 1000 lo
        err_pkt.push_back(static_cast<char>(0x03)); // errno 1000 hi
        err_pkt.push_back('#');
        err_pkt.append(reinterpret_cast<char*>(sqlstate), 5);
        err_pkt.append(errmsg);
        conn->sendInLoop(err_pkt.data(), err_pkt.size());
        return;
    }

    // 4. 转发客户端请求到后端
    if (!backend->sendRaw(data.data(), data.size())) {
        LOG_ERROR("Backend sendRaw failed: {}", backend->lastError());
        METRICS_INC("backend_send_errors");
        PoolManager::instance().returnConnection(pool_name, backend);
        return;
    }

    // 5. 读取后端响应并转发给客户端
    //    TODO: 用完整的 MySQL 包边界解析替代超时启发式
    constexpr size_t RECV_BUF = 64 * 1024;
    std::vector<char> resp_buf(RECV_BUF);
    bool got_any = false;

    while (true) {
        // 首包等待稍长（数据库处理可能需要几十毫秒）
        auto timeout = got_any
            ? std::chrono::milliseconds(50)
            : std::chrono::milliseconds(5000);

        ssize_t n = backend->recvRaw(resp_buf.data(), RECV_BUF, timeout);
        if (n <= 0) {
            if (!got_any) {
                LOG_WARN("Backend returned no data for request");
            }
            break;
        }

        if (!conn->sendInLoop(resp_buf.data(), static_cast<size_t>(n))) {
            LOG_WARN("Client send failed, aborting forwarding");
            break;
        }
        got_any = true;
    }

    // 6. 归还连接
    PoolManager::instance().returnConnection(pool_name, backend);

    // 7. 延迟指标
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    METRICS_LATENCY("query_latency", elapsed);
    METRICS_GAUGE_SET("avg_latency_ms", static_cast<double>(elapsed.count()));
}

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
        static_cast<size_t>(config.pool.max_connections)
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

    g_server->setConnectionCallback([](auto conn) {
        LOG_DEBUG("New connection: {}:{}", conn->remoteIp(), conn->remotePort());
        METRICS_INC("connections_total");
        METRICS_GAUGE_SET("active_connections",
            Metrics::instance().getGauge("active_connections") + 1);
    });

    // Fix-1: 实际转发逻辑
    auto pool_timeout = std::chrono::milliseconds(config.pool.connection_timeout_ms);
    g_server->setMessageCallback(
        [&workers, pool_timeout, pool_name]
        (auto conn, const char* data, size_t len) {
            // 拷贝数据后立即投递，不阻塞 epoll 线程
            std::vector<char> buf(data, data + len);
            workers->post([conn, buf = std::move(buf), pool_timeout, pool_name]() mutable {
                forwardRequest(conn, std::move(buf), pool_name, pool_timeout);
            });
        }
    );

    g_server->setCloseCallback([](auto conn) {
        LOG_DEBUG("Connection closed: {}", conn->remoteIp());
        METRICS_GAUGE_SET("active_connections",
            Metrics::instance().getGauge("active_connections") - 1);
    });

    // ---- 信号处理 ----
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
