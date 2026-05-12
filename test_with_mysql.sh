#!/bin/bash
# DB-Proxy MySQL 测试脚本
#
# 支持两种模式：
#   --mode local   (默认) 连接本地 MySQL
#   --mode docker  通过 Docker 启动 MySQL 容器
#
# 用法：
#   ./test_with_mysql.sh                  # 默认 local 模式：检测 MySQL → 建表 → 编译 → examples + test_pool(MySQL)
#   ./test_with_mysql.sh --mode docker    # Docker 模式
#   ./test_with_mysql.sh --mode local     # 显式 local 模式
#   ./test_with_mysql.sh --also-pg        # 在 MySQL 用例之后额外跑 PG 用例（需本机 PG）
#   ./test_with_mysql.sh stop             # 停止 Docker 容器（仅 docker 模式需要）

set -e

# ============================================================================
# 颜色定义
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ============================================================================
# 配置（可通过环境变量覆盖）
# ============================================================================
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-}"
MYSQL_DATABASE="${MYSQL_DATABASE:-test}"

# Docker 模式专用配置
DOCKER_MYSQL_PASSWORD="${DOCKER_MYSQL_PASSWORD:-rootpassword}"
DOCKER_CONTAINER_NAME="db-proxy-mysql"

# ============================================================================
# 参数解析
# ============================================================================
MODE="local"
ALSO_PG=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --also-pg)
            ALSO_PG=1
            shift
            ;;
        stop)
            stop_docker
            exit 0
            ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --mode local    连接本地 MySQL（默认）"
            echo "  --mode docker   通过 Docker 启动 MySQL"
            echo "  --also-pg       MySQL 用例后再运行 PostgreSQL 场景化用例（可选）"
            echo "                  （连接池集成测试 test_pool 在 MySQL 用例后始终以 DBPROXY_TEST_MYSQL=1 运行）"
            echo "  stop            停止 Docker 容器"
            echo "  -h, --help      显示帮助"
            echo ""
            echo "环境变量（local 模式）:"
            echo "  MYSQL_HOST      MySQL 主机（默认 127.0.0.1）"
            echo "  MYSQL_PORT      MySQL 端口（默认 3306）"
            echo "  MYSQL_USER      MySQL 用户（默认 root）"
            echo "  MYSQL_PASSWORD   MySQL 密码（默认空）"
            echo "  MYSQL_DATABASE   MySQL 数据库（默认 test）"
            exit 0
            ;;
        *)
            echo -e "${RED}未知参数: $1${NC}"
            exit 1
            ;;
    esac
done

# ============================================================================
# 函数：检测本地 MySQL
# ============================================================================
check_local_mysql() {
    echo -e "${YELLOW}[检测] 检查本地 MySQL...${NC}"

    # 检查 mysql 客户端
    if ! command -v mysql &> /dev/null; then
        echo -e "${RED}  ✗ mysql 客户端未安装${NC}"
        echo -e "${CYAN}  安装方式: brew install mysql-client${NC}"
        return 1
    fi

    echo -e "${GREEN}  ✓ mysql 客户端已安装: $(mysql --version 2>&1 | head -1)${NC}"

    # 构建连接参数
    local CONNECT_ARGS="-h ${MYSQL_HOST} -P ${MYSQL_PORT} -u ${MYSQL_USER}"
    if [[ -n "${MYSQL_PASSWORD}" ]]; then
        CONNECT_ARGS+=" -p${MYSQL_PASSWORD}"
    fi

    # 尝试连接
    if mysql ${CONNECT_ARGS} -e "SELECT 1" &> /dev/null; then
        echo -e "${GREEN}  ✓ MySQL 连接成功！${NC}"
        echo -e "${CYAN}    主机: ${MYSQL_HOST}:${MYSQL_PORT}  用户: ${MYSQL_USER}  数据库: ${MYSQL_DATABASE}${NC}"

        # 显示 MySQL 版本
        local version
        version=$(mysql ${CONNECT_ARGS} -e "SELECT VERSION()" -s -N 2>/dev/null || echo "unknown")
        echo -e "${CYAN}    版本: ${version}${NC}"
        return 0
    else
        echo -e "${RED}  ✗ MySQL 连接失败${NC}"
        echo -e "${CYAN}  请确认 MySQL 服务已启动：${NC}"
        echo -e "${CYAN}    brew services start mysql${NC}"
        echo -e "${CYAN}  或设置正确的环境变量：${NC}"
        echo -e "${CYAN}    MYSQL_HOST / MYSQL_PORT / MYSQL_USER / MYSQL_PASSWORD${NC}"
        return 1
    fi
}

# ============================================================================
# 函数：启动 Docker MySQL
# ============================================================================
start_docker_mysql() {
    echo -e "${YELLOW}[Docker] 启动 MySQL 容器...${NC}"

    # 检查 Docker
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}  ✗ Docker 未安装${NC}"
        return 1
    fi

    # 检查容器是否已存在
    if docker ps -a --format '{{.Names}}' | grep -q "^${DOCKER_CONTAINER_NAME}$"; then
        echo -e "${YELLOW}  容器已存在，正在启动...${NC}"
        docker start ${DOCKER_CONTAINER_NAME} &> /dev/null || true
    else
        docker run -d --name ${DOCKER_CONTAINER_NAME} \
            -e MYSQL_ROOT_PASSWORD=${DOCKER_MYSQL_PASSWORD} \
            -e MYSQL_DATABASE=${MYSQL_DATABASE} \
            -p ${MYSQL_PORT}:3306 \
            mysql:8.0 --default-authentication-plugin=mysql_native_password
    fi

    echo -e "${YELLOW}  等待 MySQL 启动...${NC}"
    sleep 10

    # 等待就绪
    local CONNECT_ARGS="-h 127.0.0.1 -P ${MYSQL_PORT} -u root -p${DOCKER_MYSQL_PASSWORD}"
    for i in $(seq 1 30); do
        if mysql ${CONNECT_ARGS} -e "SELECT 1" &> /dev/null; then
            echo -e "${GREEN}  ✓ MySQL Docker 已就绪！${NC}"
            # Docker 模式覆盖密码
            MYSQL_PASSWORD="${DOCKER_MYSQL_PASSWORD}"
            MYSQL_USER="root"
            return 0
        fi
        echo "  等待中... ($i/30)"
        sleep 2
    done

    echo -e "${RED}  ✗ MySQL Docker 启动超时${NC}"
    return 1
}

# ============================================================================
# 函数：停止 Docker MySQL
# ============================================================================
stop_docker() {
    echo -e "${YELLOW}[Docker] 停止 MySQL 容器...${NC}"
    if docker ps -a --format '{{.Names}}' | grep -q "^${DOCKER_CONTAINER_NAME}$"; then
        docker rm -f ${DOCKER_CONTAINER_NAME} &> /dev/null
        echo -e "${GREEN}  ✓ 容器已移除${NC}"
    else
        echo -e "${YELLOW}  容器不存在，无需清理${NC}"
    fi
}

# ============================================================================
# 函数：初始化测试数据库
# ============================================================================
init_database() {
    echo -e "${YELLOW}[初始化] 创建测试数据...${NC}"

    local CONNECT_ARGS="-h ${MYSQL_HOST} -P ${MYSQL_PORT} -u ${MYSQL_USER}"
    if [[ -n "${MYSQL_PASSWORD}" ]]; then
        CONNECT_ARGS+=" -p${MYSQL_PASSWORD}"
    fi

    mysql ${CONNECT_ARGS} -e "
        CREATE DATABASE IF NOT EXISTS ${MYSQL_DATABASE};
        USE ${MYSQL_DATABASE};
        CREATE TABLE IF NOT EXISTS users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(100),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        INSERT INTO users (name, email) VALUES
            ('Alice', 'alice@example.com'),
            ('Bob', 'bob@example.com'),
            ('Charlie', 'charlie@example.com')
        ON DUPLICATE KEY UPDATE name=VALUES(name);
    " 2>/dev/null

    echo -e "${GREEN}  ✓ 测试数据创建完成${NC}"
}

# ============================================================================
# 函数：编译项目
# ============================================================================
build_project() {
    echo -e "${YELLOW}[编译] 构建 db-proxy...${NC}"

    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

    echo -e "${GREEN}  ✓ 编译完成${NC}"
    cd ..
}

# ============================================================================
# 函数：运行测试
# ============================================================================
run_tests() {
    echo -e "${YELLOW}[测试] 运行场景化用例...${NC}"

    # 设置环境变量供 example 程序使用
    export MYSQL_HOST
    export MYSQL_PORT
    export MYSQL_USER
    export MYSQL_PASSWORD
    export MYSQL_DATABASE

    cd build

    # 运行 MySQL 示例
    if [[ -f ./examples ]]; then
        echo ""
        echo -e "${CYAN}========================================"
        echo "   MySQL 场景化用例"
        echo -e "========================================${NC}"
        ./examples || true
    fi

    # PostgreSQL 用例请使用 ./test_with_pg.sh；此处仅在显式传入 --also-pg 时附加运行
    if [[ "${ALSO_PG}" -eq 1 && -f ./examples_pg ]]; then
        echo ""
        echo -e "${CYAN}========================================"
        echo "   PostgreSQL 场景化用例（--also-pg）"
        echo -e "========================================${NC}"
        ./examples_pg || true
    fi

    # 连接池集成测试（需 DBPROXY_TEST_MYSQL=1；本脚本已确认 MySQL 可达）
    if [[ -f ./test_pool ]]; then
        echo ""
        echo -e "${CYAN}========================================"
        echo "   MySQL 连接池集成测试 (test_pool)"
        echo -e "========================================${NC}"
        DBPROXY_TEST_MYSQL=1 ./test_pool || true
    fi

    cd ..
}

# ============================================================================
# 主流程
# ============================================================================
main() {
    echo -e "${CYAN}========================================"
    echo "   DB-Proxy 数据库联动测试"
    echo -e "========================================${NC}"
    echo -e "  模式: ${MODE}"
    echo ""

    # 1. 连接/启动数据库
    case "${MODE}" in
        local)
            if ! check_local_mysql; then
                echo ""
                echo -e "${YELLOW}提示：可使用 Docker 模式：$0 --mode docker${NC}"
                exit 1
            fi
            ;;
        docker)
            if ! start_docker_mysql; then
                exit 1
            fi
            ;;
        *)
            echo -e "${RED}未知模式: ${MODE}（可选: local / docker）${NC}"
            exit 1
            ;;
    esac

    # 2. 初始化测试数据
    init_database

    # 3. 编译项目
    build_project

    # 4. 运行测试
    run_tests

    echo ""
    echo -e "${GREEN}========================================"
    echo "   测试完成！"
    echo -e "========================================${NC}"

    if [[ "${MODE}" == "docker" ]]; then
        echo -e "${YELLOW}  清理 Docker: $0 stop${NC}"
    fi
}

main
