#include "pool/connection_pool.h"
#include <iostream>
#include <chrono>
#include <vector>

using namespace dbproxy;

int main() {
    std::cout << "=== Connection Pool Benchmark ===" << std::endl;
    
    // 创建连接池
    ConnectionPool pool("127.0.0.1", 3306, "root", "", "test", 10, 100,
                       std::chrono::seconds(30), std::chrono::seconds(5));
    
    pool.warmup();
    
    // 基准测试：单线程获取连接
    std::cout << "\n--- Sequential Test ---" << std::endl;
    {
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < 1000; ++i) {
            auto conn = pool.getConnection(std::chrono::seconds(1));
            if (conn) {
                pool.returnConnection(conn);
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "1000 sequential gets: " << duration.count() << " us" << std::endl;
        std::cout << "Average per get: " << duration.count() / 1000.0 << " us" << std::endl;
    }
    
    return 0;
}
