#!/bin/bash

# DB-Proxy PostgreSQL 测试脚本

set -e

echo "========================================"
echo "  DB-Proxy PostgreSQL 测试环境"
echo "========================================"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查 Docker
check_docker() {
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}错误: Docker 未安装${NC}"
        exit 1
    fi
    
    if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
        echo -e "${RED}错误: Docker Compose 未安装${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✓ Docker 环境检查通过${NC}"
}

# 启动 PostgreSQL
start_postgres() {
    echo -e "\n${YELLOW}[1/4] 启动 PostgreSQL 容器...${NC}"
    
    if [ -f docker-compose-pg.yml ]; then
        docker-compose -f docker-compose-pg.yml up -d
    elif docker compose version &> /dev/null; then
        docker compose -f docker-compose-pg.yml up -d
    else
        docker-compose -f docker-compose-pg.yml up -d
    fi
    
    # 等待 PostgreSQL 就绪
    echo -e "${YELLOW}等待 PostgreSQL 启动...${NC}"
    for i in {1..30}; do
        if docker exec db-proxy-pg pg_isready -U postgres &> /dev/null; then
            echo -e "${GREEN}✓ PostgreSQL 已就绪${NC}"
            return 0
        fi
        sleep 1
    done
    
    echo -e "${RED}✗ PostgreSQL 启动超时${NC}"
    exit 1
}

# 运行初始化脚本
init_database() {
    echo -e "\n${YELLOW}[2/4] 初始化数据库...${NC}"
    
    if [ -f init-pg.sql ]; then
        docker exec -i db-proxy-pg psql -U postgres < init-pg.sql
        echo -e "${GREEN}✓ 数据库初始化完成${NC}"
    else
        echo -e "${YELLOW}⚠ 初始化脚本不存在，跳过${NC}"
    fi
}

# 测试连接
test_connection() {
    echo -e "\n${YELLOW}[3/4] 测试 PostgreSQL 连接...${NC}"
    
    # 测试基本连接
    if docker exec db-proxy-pg psql -U postgres -c "SELECT version();" &> /dev/null; then
        echo -e "${GREEN}✓ PostgreSQL 连接测试通过${NC}"
    else
        echo -e "${RED}✗ PostgreSQL 连接测试失败${NC}"
        exit 1
    fi
    
    # 测试数据库
    docker exec db-proxy-pg psql -U postgres -d postgres -c "SELECT current_database();" &> /dev/null
    echo -e "${GREEN}✓ 数据库查询测试通过${NC}"
}

# 显示连接信息
show_info() {
    echo -e "\n${YELLOW}[4/4] 连接信息${NC}"
    echo "========================================"
    echo "PostgreSQL 连接参数:"
    echo "  主机: 127.0.0.1"
    echo "  端口: 5432"
    echo "  用户: postgres"
    echo "  密码: postgres"
    echo "  数据库: test"
    echo ""
    echo "可选 - pgAdmin:"
    echo "  地址: http://localhost:5050"
    echo "  邮箱: admin@example.com"
    echo "  密码: admin"
    echo "========================================"
}

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理资源...${NC}"
    if [ -f docker-compose-pg.yml ]; then
        docker-compose -f docker-compose-pg.yml down
    else
        docker compose -f docker-compose-pg.yml down
    fi
    echo -e "${GREEN}✓ 清理完成${NC}"
}

# 主流程
main() {
    case "${1:-start}" in
        start)
            check_docker
            start_postgres
            init_database
            test_connection
            show_info
            ;;
        stop)
            cleanup
            ;;
        restart)
            cleanup
            sleep 2
            start_postgres
            init_database
            test_connection
            show_info
            ;;
        test)
            check_docker
            test_connection
            ;;
        *)
            echo "用法: $0 {start|stop|restart|test}"
            echo "  start   - 启动 PostgreSQL 并初始化"
            echo "  stop    - 停止并清理容器"
            echo "  restart - 重启服务"
            echo "  test    - 仅测试连接"
            exit 1
            ;;
    esac
}

main "$@"
