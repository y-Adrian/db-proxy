cmake_minimum_required(VERSION 3.20)
project(db-proxy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译器优化选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native")

# 头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# 源码目录
set(CORE_SOURCES
    src/core/logger.cpp
    src/core/config.cpp
)

set(NETWORK_SOURCES
    src/network/epoll_server.cpp
    src/network/tcp_connection.cpp
    src/network/event_loop.cpp
)

set(PROTOCOL_SOURCES
    src/protocol/mysql_packet.cpp
    src/protocol/mysql_parser.cpp
    src/protocol/protocol_handler.cpp
)

set(POOL_SOURCES
    src/pool/connection.cpp
    src/pool/connection_pool.cpp
    src/pool/pool_manager.cpp
)

set(MONITOR_SOURCES
    src/monitor/metrics.cpp
    src/monitor/statistics.cpp
    src/monitor/performance_analyzer.cpp
)

# 主程序
add_executable(db-proxy
    src/main.cpp
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${PROTOCOL_SOURCES}
    ${POOL_SOURCES}
    ${MONITOR_SOURCES}
)

# 单元测试
enable_testing()
add_executable(test_pool tests/test_connection_pool.cpp ${POOL_SOURCES} ${CORE_SOURCES})
add_test(NAME PoolTest COMMAND test_pool)

# 性能测试
add_executable(bench_pool benchmarks/bench_connection_pool.cpp ${POOL_SOURCES} ${CORE_SOURCES})

# 场景化用例
add_executable(examples examples/examples.cpp ${POOL_SOURCES} ${MONITOR_SOURCES} ${CORE_SOURCES})

# 压力测试
add_executable(stress_test tests/stress_test.cpp ${POOL_SOURCES} ${CORE_SOURCES})

# 链接库
target_link_libraries(db-proxy pthread)
target_link_libraries(examples pthread)
target_link_libraries(stress_test pthread)
