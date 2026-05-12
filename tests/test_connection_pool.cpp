#include "pool/connection_pool.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>

using namespace dbproxy;

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};

void worker(ConnectionPool& pool) {
    for (int i = 0; i < 10; ++i) {
        auto conn = pool.getConnection(std::chrono::seconds(5));
        if (conn) {
            // 模拟查询
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pool.returnConnection(conn);
            success_count++;
        } else {
            fail_count++;
        }
    }
}

int main() {
    std::cout << "=== Connection Pool Test ===" << std::endl;

    // 避免测试环境未启动 MySQL 时，写 socket 触发 SIGPIPE 直接终止进程。
    std::signal(SIGPIPE, SIG_IGN);

    // 这是一个“集成测试”：需要本机 127.0.0.1:3306 有可用 MySQL。
    // CI/沙箱环境通常没有数据库，因此默认跳过；需要时可手动启用：
    //   DBPROXY_TEST_MYSQL=1 ctest --output-on-failure
    const char* enable = std::getenv("DBPROXY_TEST_MYSQL");
    if (!enable || std::string(enable) != "1") {
        std::cout << "[SKIP] MySQL not enabled (set DBPROXY_TEST_MYSQL=1 to run)\n";
        return 0;
    }
    
    // 创建连接池
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 5, 50,
                       std::chrono::seconds(30), std::chrono::seconds(5));
    
    // 预热
    if (!pool.warmup()) {
        std::cerr << "Failed to warmup pool" << std::endl;
        return 1;
    }
    
    std::cout << "Pool warmed up: " << pool.totalConnections() << " connections" << std::endl;
    
    // 并发测试
    const int num_threads = 20;
    std::vector<std::thread> threads;
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, std::ref(pool));
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // 输出结果
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "Success: " << success_count.load() << std::endl;
    std::cout << "Failed: " << fail_count.load() << std::endl;
    std::cout << "Total: " << (success_count.load() + fail_count.load()) << std::endl;
    std::cout << "QPS: " << (success_count.load() * 1000.0 / duration.count()) << std::endl;
    std::cout << "Pool stats: total=" << pool.totalConnections() 
              << " idle=" << pool.idleConnections()
              << " busy=" << pool.busyConnections() << std::endl;
    
    return 0;
}
