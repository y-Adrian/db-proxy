#!/bin/bash
# DB-Proxy 快速测试脚本

set -e

echo "========================================"
echo "   DB-Proxy 数据库联动测试"
echo "========================================"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查 MySQL
check_mysql() {
    echo -e "${YELLOW}检查 MySQL 连接...${NC}"
    
    if command -v mysql &> /dev/null; then
        if mysql -h 127.0.0.1 -u root -prootpassword -e "SELECT 1" 2>/dev/null; then
            echo -e "${GREEN}MySQL 连接成功！${NC}"
            return 0
        fi
    fi
    
    echo -e "${RED}MySQL 未运行或无法连接${NC}"
    echo -e "${YELLOW}启动 MySQL...${NC}"
    docker run -d --name db-proxy-mysql \
        -e MYSQL_ROOT_PASSWORD=rootpassword \
        -e MYSQL_DATABASE=test \
        -p 3306:3306 \
        mysql:8.0 --default-authentication-plugin=mysql_native_password
    
    echo "等待 MySQL 启动..."
    sleep 15
    
    # 等待 MySQL 就绪
    for i in {1..30}; do
        if mysql -h 127.0.0.1 -u root -prootpassword -e "SELECT 1" 2>/dev/null; then
            echo -e "${GREEN}MySQL 已就绪！${NC}"
            return 0
        fi
        echo "等待 MySQL... ($i/30)"
        sleep 2
    done
    
    echo -e "${RED}MySQL 启动失败${NC}"
    return 1
}

# 编译项目
build_project() {
    echo -e "${YELLOW}编译项目...${NC}"
    
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    
    echo -e "${GREEN}编译完成！${NC}"
}

# 运行测试
run_tests() {
    echo -e "${YELLOW}运行测试...${NC}"
    
    # 创建测试数据库
    mysql -h 127.0.0.1 -u root -prootpassword -e "
        CREATE DATABASE IF NOT EXISTS test;
        USE test;
        CREATE TABLE IF NOT EXISTS users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(100),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        INSERT INTO users (name, email) VALUES 
            ('Alice', 'alice@example.com'),
            ('Bob', 'bob@example.com'),
            ('Charlie', 'charlie@example.com');
    "
    
    echo -e "${GREEN}测试数据创建完成${NC}"
    
    # 运行用例测试
    echo ""
    echo "========================================"
    echo "   运行场景化用例"
    echo "========================================"
    ./examples
    
    echo ""
    echo "========================================"
    echo "   压力测试"
    echo "========================================"
    
    # 启用压力测试（需要取消注释 examples.cpp 中的数据库相关测试）
    # ./stress_test
}

# 主流程
main() {
    check_mysql
    build_project
    run_tests
    
    echo ""
    echo -e "${GREEN}========================================"
    echo "   测试完成！"
    echo "========================================${NC}"
}

main "$@"
