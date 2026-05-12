cmake_minimum_required(VERSION 3.20)
project(db-proxy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# OpenSSL 用于 PostgreSQL SCRAM/MD5、MySQL caching_sha2 等高级认证。
# 为了在最小环境也能编译（先打通 MySQL mysql_native_password），这里把 OpenSSL 设为可选。
option(DBPROXY_ENABLE_POSTGRES "Enable PostgreSQL backend connection implementation" ON)
find_package(OpenSSL QUIET)
if (NOT OpenSSL_FOUND AND DBPROXY_ENABLE_POSTGRES)
    message(WARNING "OpenSSL not found; disabling PostgreSQL support for this build.")
    set(DBPROXY_ENABLE_POSTGRES OFF)
endif()

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
    src/crypto/sha1.cpp
)

set(NETWORK_SOURCES
    src/network/epoll_server.cpp
    src/network/tcp_connection.cpp
    src/network/event_loop.cpp
)

set(PROTOCOL_SOURCES
    src/protocol/mysql_packet.cpp
    src/protocol/mysql_parser.cpp
    src/protocol/mysql_relay.cpp
    src/protocol/transparent_session_relay.cpp
    src/protocol/protocol_handler.cpp
)

set(POOL_SOURCES
    src/pool/connection.cpp
    src/pool/connection_pool.cpp
    src/pool/pool_manager.cpp
)

if (DBPROXY_ENABLE_POSTGRES)
    add_compile_definitions(DBPROXY_ENABLE_POSTGRES=1)
    list(APPEND POOL_SOURCES src/pool/postgresql_connection.cpp)
else()
    add_compile_definitions(DBPROXY_ENABLE_POSTGRES=0)
endif()

set(MONITOR_SOURCES
    src/monitor/metrics.cpp
    src/monitor/statistics.cpp
    src/monitor/performance_analyzer.cpp
)

set(DIAGNOSTICS_SOURCES
    src/diagnostics/mock_provider.cpp
    src/diagnostics/ollama_provider.cpp
    src/diagnostics/prompt_builder.cpp
    src/diagnostics/diagnostic_engine.cpp
    src/diagnostics/report_generator.cpp
)

set(CLI_SOURCES
    src/cli/connection_config.cpp
    src/cli/shell.cpp
    src/cli/main.cpp
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

add_executable(test_pooled_session_relay
    tests/test_pooled_session_relay.cpp
    src/protocol/transparent_session_relay.cpp
    src/core/logger.cpp
    src/core/config.cpp
)
add_test(NAME PooledSessionRelayTest COMMAND test_pooled_session_relay)
target_link_libraries(test_pooled_session_relay PRIVATE Threads::Threads)

# 性能测试
add_executable(bench_pool benchmarks/bench_connection_pool.cpp ${POOL_SOURCES} ${CORE_SOURCES})

# 场景化用例
add_executable(examples examples/examples.cpp ${POOL_SOURCES} ${MONITOR_SOURCES} ${CORE_SOURCES})
if (DBPROXY_ENABLE_POSTGRES)
    add_executable(examples_pg examples/examples_pg.cpp ${POOL_SOURCES} ${MONITOR_SOURCES} ${CORE_SOURCES})
endif()

# CLI 工具
add_executable(dbcli ${CLI_SOURCES} ${POOL_SOURCES} ${MONITOR_SOURCES} ${CORE_SOURCES})

# 压力测试
add_executable(stress_test tests/stress_test.cpp ${POOL_SOURCES} ${CORE_SOURCES})

# 诊断模块示例（支持 Mock / Ollama 模式）
add_executable(diagnostics_demo
    examples/diagnostics_demo.cpp
    ${DIAGNOSTICS_SOURCES}
    ${MONITOR_SOURCES}
    ${POOL_SOURCES}
    ${CORE_SOURCES}
)

# 链接库
target_link_libraries(db-proxy PRIVATE Threads::Threads)
target_link_libraries(dbcli PRIVATE Threads::Threads)
target_link_libraries(examples PRIVATE Threads::Threads)
if (TARGET examples_pg)
    target_link_libraries(examples_pg PRIVATE Threads::Threads)
endif()
target_link_libraries(stress_test PRIVATE Threads::Threads)
target_link_libraries(test_pool PRIVATE Threads::Threads)
target_link_libraries(bench_pool PRIVATE Threads::Threads)
target_link_libraries(diagnostics_demo PRIVATE Threads::Threads)

if (OpenSSL_FOUND)
    foreach(t db-proxy dbcli examples stress_test test_pool test_pooled_session_relay bench_pool diagnostics_demo)
        if (TARGET ${t})
            target_link_libraries(${t} PRIVATE OpenSSL::Crypto)
        endif()
    endforeach()
    if (TARGET examples_pg)
        target_link_libraries(examples_pg PRIVATE OpenSSL::Crypto)
    endif()
endif()
