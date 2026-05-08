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

```bash
# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 运行
./db-proxy
```

## 数据库支持

### MySQL

```bash
# 启动 MySQL 测试环境
docker-compose -f docker-compose.yml up -d

# 运行 MySQL 用例
./examples

# MySQL 默认配置
host: 127.0.0.1
port: 3306
user: root
password: (empty)
database: test
```

### PostgreSQL

```bash
# 启动 PostgreSQL 测试环境
docker-compose -f docker-compose-pg.yml up -d

# 运行 PostgreSQL 用例
./examples_pg

# PostgreSQL 默认配置
host: 127.0.0.1
port: 5432
user: postgres
password: postgres
database: test
```

## 功能示例

### MySQL 用例

```bash
# 查看 MySQL 用例
cat examples/examples.cpp
```

### PostgreSQL 用例

```bash
# 查看 PostgreSQL 用例
cat examples/examples_pg.cpp
```

主要场景包括：
- 基础连接池使用
- 协议特性演示
- 事务处理
- 连接参数配置
- LISTEN/NOTIFY 机制
- COPY 批量导入
- 健康检查
- 多数据库管理
- 性能监控

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
