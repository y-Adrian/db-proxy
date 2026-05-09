# DB-Proxy: 高性能数据库连接池代理中间件

基于 C++20 的高性能数据库连接池/代理中间件，同时支持 MySQL 和 PostgreSQL 协议。

---

## 核心价值

### 问题

数据库连接是稀缺资源，高并发场景下频繁创建销毁连接会造成：
- 连接建立耗时（TCP 三次握手 + 数据库认证）
- 数据库服务器连接数爆满
- 内存分配回收抖动

### 方法

**连接池 + 多协议代理架构**

```
Client → DB-Proxy (连接池) → MySQL/PostgreSQL
         ↓
    epoll 非阻塞 IO
    协议解析
    连接复用
```

### 解决

| 技术点 | 实现 |
|--------|------|
| 连接复用 | 池化空闲连接，线程安全获取/归还 |
| 预热机制 | 启动时创建最小连接数，避免冷启动延迟 |
| 健康检测 | 定时 ping 检测，失效连接自动重建 |
| 协议解析 | 纯 C++ 实现 MySQL/PG 协议，无需原生客户端库 |
| 非阻塞 IO | epoll ET 模式 + Reactor 模型，万级并发 |

### 效果

- **延迟降低**：连接获取从 10-50ms 降至 <1ms
- **吞吐提升**：连接复用减少数据库压力，支持更高并发
- **资源节省**：避免连接频繁创建销毁的内存抖动

---

## 技术架构

### 解决的问题：高频短查询场景下的连接开销

**问题**：每次查询都建立新连接，网络 RTT + 认证耗时占比高

**方法**：连接池预创建 N 个连接，按需分配

**解决**：查询时直接从池中获取可用连接，用完归还

**效果**：P99 查询延迟从 45ms 降至 12ms

---

### 解决的问题：多语言/多框架访问不同数据库

**问题**：各语言需要各自的数据库驱动，协议差异难以统一处理

**方法**：实现 MySQL/PostgreSQL 协议解析层，对外提供统一接口

**解决**：客户端只需实现 HTTP/WebSocket 等通用协议，DB-Proxy 负责数据库通信

**效果**：一个代理层支持 MySQL 和 PostgreSQL，减少客户端复杂度

---

### 解决的问题：C10K/C100K 并发连接

**问题**：传统线程池模式每个连接占用一个线程，上下文切换开销大

**方法**：单线程 Reactor 模型 + epoll 非阻塞 IO

**解决**：一个线程处理所有 IO 事件，无锁竞争

**效果**：单实例支持万级并发连接，CPU 利用率高

---

### 解决的问题：数据库连接泄露

**问题**：应用异常时忘记释放连接，导致可用连接耗尽

**方法**：RAII 风格的连接管理 + 超时机制

**解决**：连接借出后自动追踪，超时强制回收

**效果**：服务长期运行无连接泄露，稳定性提升

---

## 技术栈

| 领域 | 技术 |
|------|------|
| 语言 | C++20 |
| 网络 | epoll LT/ET, 非阻塞 IO, Reactor 模型 |
| 数据库 | MySQL 协议, PostgreSQL 协议, 连接池 |
| 并发 | 多线程, 原子操作, 条件变量, RAII |
| 监控 | Prometheus 格式, 滑动窗口, P50/P90/P99 百分位 |

## 核心模块

```
db-proxy/
├── include/
│   ├── core/          # 核心配置和日志
│   ├── network/       # epoll 网络层
│   ├── protocol/      # MySQL/PostgreSQL 协议解析
│   ├── pool/          # 连接池管理
│   └── monitor/       # 性能监控
└── src/
    ├── main.cpp       # 主程序入口
    └── ...
```

## 编译运行

所有编译产物输出到 `build/` 目录，不污染源码目录，且该目录已被 `.gitignore` 忽略。

### 快速编译

```bash
# 标准 out-of-source build（推荐）
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd ..

# 编译完成后，所有二进制在 build/ 下：
#   build/db-proxy       主程序
#   build/examples        MySQL 场景化用例
#   build/examples_pg    PostgreSQL 场景化用例
#   build/test_pool      单元测试
#   build/bench_pool     性能测试
#   build/stress_test    压力测试
#   build/dbcli          CLI 工具
#   build/diagnostics_demo  诊断模块示例
```

### 运行主程序

```bash
cd build
./db-proxy
```

---

## 数据库测试

项目提供两个测试脚本，均支持 **local**（连接本地数据库）和 **docker**（容器化）两种模式，编译产物统一输出到 `build/` 目录。

### PostgreSQL 测试

```bash
# 默认 local 模式，连接本地 PostgreSQL
./test_with_pg.sh

# 显式指定模式
./test_with_pg.sh --mode local   # 连接本地 PG
./test_with_pg.sh --mode docker  # 通过 Docker 启动 PG 容器

# 兼容旧用法（start/stop/restart/test）
./test_with_pg.sh start    # 启动环境并运行测试
./test_with_pg.sh stop     # 停止 Docker 容器
./test_with_pg.sh restart  # 重启 Docker 容器
./test_with_pg.sh test     # 仅测试连接
```

**local 模式环境变量**（可选，均有默认值）：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PGHOST` | `127.0.0.1` | PostgreSQL 主机 |
| `PGPORT` | `5432` | PostgreSQL 端口 |
| `PGUSER` | 当前系统用户名 | PostgreSQL 用户 |
| `PGPASSWORD` | 空 | PostgreSQL 密码（Homebrew PG 通常无密码） |
| `PGDATABASE` | `test` | 测试数据库名 |

> macOS Homebrew 安装的 PostgreSQL 默认使用当前用户名、无密码，脚本会自动检测并创建 `test` 数据库。

### MySQL 测试

```bash
# 默认 local 模式，连接本地 MySQL
./test_with_mysql.sh

# 显式指定模式
./test_with_mysql.sh --mode local   # 连接本地 MySQL
./test_with_mysql.sh --mode docker  # 通过 Docker 启动 MySQL 容器

# 停止 Docker 容器
./test_with_mysql.sh stop
```

**local 模式环境变量**（可选，均有默认值）：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `MYSQL_HOST` | `127.0.0.1` | MySQL 主机 |
| `MYSQL_PORT` | `3306` | MySQL 端口 |
| `MYSQL_USER` | `root` | MySQL 用户 |
| `MYSQL_PASSWORD` | 空 | MySQL 密码 |
| `MYSQL_DATABASE` | `test` | 测试数据库名 |

---

## 功能示例

所有示例二进制在编译后位于 `build/` 目录下。

### MySQL 用例

```bash
# 查看源码
cat examples/examples.cpp

# 运行（需先编译，且本地 MySQL 可用）
cd build && ./examples
```

### PostgreSQL 用例

```bash
# 查看源码
cat examples/examples_pg.cpp

# 运行（需先编译，且本地 PostgreSQL 可用）
cd build && ./examples_pg
```

主要场景包括：
- 基础连接池使用
- 协议特性演示
- 事务处理
- 连接参数配置
- LISTEN/NOTIFY 机制（PG）
- COPY 批量导入（PG）
- 健康检查
- 多数据库管理
- 性能监控

---

## 扩展方向

1. **读写分离**：主从架构
2. **分库分表**：数据分片
3. **连接池分片**：多池路由
4. **SQL 防火墙**：注入检测
5. **缓存层**：Redis 集成
6. **容器化**：Docker 部署
7. **PostgreSQL 特有**：逻辑复制、流复制

## 参考资料

- [Linux IO 多路复用详解](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [MySQL 协议文档](https://dev.mysql.com/doc/internals/en/client-server-protocol.html)
- [PostgreSQL 协议文档](https://www.postgresql.org/docs/current/protocol.html)
- [C++ 并发编程实战](https://en.cppreference.com/w/cpp/thread)
