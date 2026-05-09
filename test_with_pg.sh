#!/bin/bash
# DB-Proxy PostgreSQL 测试脚本
#
# 支持两种模式：
#   --mode local   (默认) 连接本地 PostgreSQL
#   --mode docker  通过 Docker Compose 启动 PostgreSQL 容器
#
# 用法：
#   ./test_with_pg.sh                     # 默认 local 模式
#   ./test_with_pg.sh --mode docker       # Docker 模式
#   ./test_with_pg.sh --mode local        # 显式 local 模式
#   ./test_with_pg.sh start               # 等同于 --mode local（兼容旧用法）
#   ./test_with_pg.sh stop                # 停止 Docker 容器（仅 docker 模式需要）
#   ./test_with_pg.sh restart             # 重启 Docker 容器
#
# 环境变量（local 模式）：
#   PGHOST          PostgreSQL 主机（默认 127.0.0.1）
#   PGPORT          PostgreSQL 端口（默认 5432）
#   PGUSER          PostgreSQL 用户（默认当前系统用户名）
#   PGPASSWORD      PostgreSQL 密码（默认空，Homebrew PG 通常无密码）
#   PGDATABASE      PostgreSQL 数据库（默认 test）

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
# 配置（环境变量优先，否则使用默认值）
# ============================================================================
PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-$(whoami)}"
PGPASSWORD="${PGPASSWORD:-}"
PGDATABASE="${PGDATABASE:-test}"

# Docker 模式专用配置
DOCKER_PG_PASSWORD="postgres"
DOCKER_PG_USER="postgres"
DOCKER_CONTAINER_NAME="db-proxy-pg"

# ============================================================================
# 参数解析
# ============================================================================
MODE="local"
ACTION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        start|stop|restart|test)
            ACTION="$1"
            shift
            ;;
        -h|--help)
            echo "用法: $0 [选项] [命令]"
            echo ""
            echo "选项:"
            echo "  --mode local    连接本地 PostgreSQL（默认）"
            echo "  --mode docker   通过 Docker Compose 启动 PostgreSQL"
            echo ""
            echo "命令:"
            echo "  start           启动/连接数据库（默认行为）"
            echo "  stop            停止 Docker 容器"
            echo "  restart         重启 Docker 容器"
            echo "  test            仅测试连接"
            echo ""
            echo "环境变量（local 模式）:"
            echo "  PGHOST          主机（默认 127.0.0.1）"
            echo "  PGPORT          端口（默认 5432）"
            echo "  PGUSER          用户（默认当前系统用户名）"
            echo "  PGPASSWORD      密码（默认空，Homebrew PG 通常无密码）"
            echo "  PGDATABASE      数据库（默认 test）"
            exit 0
            ;;
        *)
            echo -e "${RED}未知参数: $1${NC}"
            exit 1
            ;;
    esac
done

# 兼容旧用法：直接传 start/stop/restart/test 命令，默认为 docker 模式
if [[ -n "${ACTION}" ]]; then
    case "${ACTION}" in
        stop|restart)
            MODE="docker"
            ;;
        start)
            # start 命令在旧脚本中是 docker 模式，新脚本中让用户显式选择
            ;;
        test)
            MODE="local"  # test 适合本地
            ;;
    esac
fi

# ============================================================================
# 函数：检测本地 PostgreSQL
# ============================================================================
check_local_postgres() {
    echo -e "${YELLOW}[检测] 检查本地 PostgreSQL...${NC}"

    # 检查 psql 客户端
    if ! command -v psql &> /dev/null; then
        echo -e "${RED}  ✗ psql 客户端未安装${NC}"
        echo -e "${CYAN}  安装方式: brew install libpq${NC}"
        echo -e "${CYAN}  或完整安装: brew install postgresql@18${NC}"
        return 1
    fi

    echo -e "${GREEN}  ✓ psql 已安装: $(psql --version 2>&1 | head -1)${NC}"

    # 构建连接参数
    local PSQL_ARGS="-h ${PGHOST} -p ${PGPORT} -U ${PGUSER}"
    if [[ -n "${PGPASSWORD}" ]]; then
        export PGPASSWORD
    fi

    # 尝试连接
    if psql ${PSQL_ARGS} -d postgres -c "SELECT 1" &> /dev/null; then
        echo -e "${GREEN}  ✓ PostgreSQL 连接成功！${NC}"
        echo -e "${CYAN}    主机: ${PGHOST}:${PGPORT}  用户: ${PGUSER}  数据库: ${PGDATABASE}${NC}"

        # 显示 PG 版本
        local version
        version=$(psql ${PSQL_ARGS} -d postgres -t -A -c "SELECT version();" 2>/dev/null | head -1 || echo "unknown")
        echo -e "${CYAN}    版本: ${version}${NC}"

        # 检查 test 数据库是否存在
        local db_exists
        db_exists=$(psql ${PSQL_ARGS} -d postgres -t -A -c "SELECT 1 FROM pg_database WHERE datname='${PGDATABASE}';" 2>/dev/null || echo "")
        if [[ "${db_exists}" != "1" ]]; then
            echo -e "${YELLOW}  数据库 '${PGDATABASE}' 不存在，正在创建...${NC}"
            psql ${PSQL_ARGS} -d postgres -c "CREATE DATABASE ${PGDATABASE};" 2>/dev/null
            echo -e "${GREEN}  ✓ 数据库 '${PGDATABASE}' 已创建${NC}"
        fi

        return 0
    else
        echo -e "${RED}  ✗ PostgreSQL 连接失败${NC}"
        echo -e "${CYAN}  请确认 PostgreSQL 服务已启动：${NC}"
        echo -e "${CYAN}    brew services start postgresql@18${NC}"
        echo -e "${CYAN}  或设置正确的环境变量：${NC}"
        echo -e "${CYAN}    PGHOST / PGPORT / PGUSER / PGPASSWORD / PGDATABASE${NC}"
        return 1
    fi
}

# ============================================================================
# 函数：启动 Docker PostgreSQL
# ============================================================================
start_docker_postgres() {
    echo -e "${YELLOW}[Docker] 启动 PostgreSQL 容器...${NC}"

    if ! command -v docker &> /dev/null; then
        echo -e "${RED}  ✗ Docker 未安装${NC}"
        return 1
    fi

    # 使用 docker-compose 或 docker compose
    local COMPOSE_CMD=""
    if command -v docker-compose &> /dev/null; then
        COMPOSE_CMD="docker-compose"
    elif docker compose version &> /dev/null; then
        COMPOSE_CMD="docker compose"
    else
        echo -e "${RED}  ✗ Docker Compose 未安装${NC}"
        return 1
    fi

    echo -e "${GREEN}  ✓ Docker 环境检查通过${NC}"

    ${COMPOSE_CMD} -f docker-compose-pg.yml up -d

    # 等待就绪
    echo -e "${YELLOW}  等待 PostgreSQL 启动...${NC}"
    for i in $(seq 1 30); do
        if docker exec ${DOCKER_CONTAINER_NAME} pg_isready -U ${DOCKER_PG_USER} &> /dev/null; then
            echo -e "${GREEN}  ✓ PostgreSQL Docker 已就绪！${NC}"
            # Docker 模式覆盖连接参数
            PGUSER="${DOCKER_PG_USER}"
            PGPASSWORD="${DOCKER_PG_PASSWORD}"
            export PGPASSWORD
            return 0
        fi
        sleep 1
    done

    echo -e "${RED}  ✗ PostgreSQL Docker 启动超时${NC}"
    return 1
}

# ============================================================================
# 函数：停止 Docker PostgreSQL
# ============================================================================
stop_docker() {
    echo -e "${YELLOW}[Docker] 停止 PostgreSQL 容器...${NC}"

    local COMPOSE_CMD=""
    if command -v docker-compose &> /dev/null; then
        COMPOSE_CMD="docker-compose"
    elif docker compose version &> /dev/null; then
        COMPOSE_CMD="docker compose"
    else
        echo -e "${RED}  ✗ Docker Compose 未安装${NC}"
        return 1
    fi

    ${COMPOSE_CMD} -f docker-compose-pg.yml down
    echo -e "${GREEN}  ✓ 容器已停止${NC}"
}

# ============================================================================
# 函数：初始化测试数据库
# ============================================================================
init_database() {
    echo -e "${YELLOW}[初始化] 创建测试数据...${NC}"

    local PSQL_ARGS="-h ${PGHOST} -p ${PGPORT} -U ${PGUSER}"
    if [[ -n "${PGPASSWORD}" ]]; then
        export PGPASSWORD
    fi

    # 执行初始化脚本
    if [[ -f "init-pg.sql" ]]; then
        psql ${PSQL_ARGS} -d ${PGDATABASE} -f init-pg.sql 2>/dev/null || \
        psql ${PSQL_ARGS} -d postgres -f init-pg.sql 2>/dev/null || true
        echo -e "${GREEN}  ✓ 初始化脚本已执行${NC}"
    fi

    # 确保基础测试数据存在（即使 init-pg.sql 部分失败）
    psql ${PSQL_ARGS} -d ${PGDATABASE} -c "
        CREATE TABLE IF NOT EXISTS users (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255) UNIQUE,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    " 2>/dev/null || true

    psql ${PSQL_ARGS} -d ${PGDATABASE} -c "
        INSERT INTO users (name, email) VALUES
            ('Alice', 'alice@example.com'),
            ('Bob', 'bob@example.com'),
            ('Charlie', 'charlie@example.com')
        ON CONFLICT (email) DO NOTHING;
    " 2>/dev/null || true

    echo -e "${GREEN}  ✓ 测试数据就绪${NC}"
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
    echo -e "${YELLOW}[测试] 运行 PostgreSQL 场景化用例...${NC}"

    # 设置环境变量供 example 程序使用
    export PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE

    cd build
    if [[ -f ./examples_pg ]]; then
        echo ""
        echo -e "${CYAN}========================================"
        echo "   PostgreSQL 场景化用例"
        echo -e "========================================${NC}"
        ./examples_pg || true
    fi
    cd ..
}

# ============================================================================
# 函数：显示连接信息
# ============================================================================
show_info() {
    echo ""
    echo -e "${CYAN}========================================"
    echo "   连接信息"
    echo -e "========================================${NC}"
    echo "  主机: ${PGHOST}"
    echo "  端口: ${PGPORT}"
    echo "  用户: ${PGUSER}"
    echo "  数据库: ${PGDATABASE}"
    if [[ "${MODE}" == "docker" ]]; then
        echo "  密码: ${PGPASSWORD}"
        echo ""
        echo "  pgAdmin: http://localhost:5050"
        echo "    邮箱: admin@example.com"
        echo "    密码: admin"
    fi
    echo -e "${CYAN}========================================${NC}"
}

# ============================================================================
# 主流程
# ============================================================================
main() {
    echo -e "${CYAN}========================================"
    echo "   DB-Proxy PostgreSQL 测试环境"
    echo -e "========================================${NC}"
    echo -e "  模式: ${MODE}"
    echo ""

    # 处理快捷命令
    if [[ "${ACTION}" == "stop" ]]; then
        stop_docker
        exit 0
    fi

    if [[ "${ACTION}" == "restart" ]]; then
        stop_docker
        sleep 2
    fi

    # 1. 连接/启动数据库
    case "${MODE}" in
        local)
            if ! check_local_postgres; then
                echo ""
                echo -e "${YELLOW}提示：可使用 Docker 模式：$0 --mode docker${NC}"
                exit 1
            fi
            ;;
        docker)
            if ! start_docker_postgres; then
                exit 1
            fi
            ;;
        *)
            echo -e "${RED}未知模式: ${MODE}（可选: local / docker）${NC}"
            exit 1
            ;;
    esac

    # 2. 仅测试连接
    if [[ "${ACTION}" == "test" ]]; then
        echo -e "${GREEN}连接测试通过${NC}"
        exit 0
    fi

    # 3. 初始化
    init_database

    # 4. 编译
    build_project

    # 5. 运行测试
    run_tests

    # 6. 显示连接信息
    show_info

    echo ""
    echo -e "${GREEN}========================================"
    echo "   测试完成！"
    echo -e "========================================${NC}"

    if [[ "${MODE}" == "docker" ]]; then
        echo -e "${YELLOW}  清理 Docker: $0 stop${NC}"
    fi
}

main
