# DB-Proxy 面试项目 - 核心技术文档

> 基于 C++20 的高性能数据库连接池代理中间件
> 仓库: https://github.com/y-Adrian/db-proxy

---

## 一、项目概述

### 1.1 功能特性

- **高性能 TCP 服务器**：基于 epoll 的异步 IO，支持万级并发连接
- **MySQL 协议解析**：完整解析 MySQL 协议包，支持 LENENC 编码
- **连接池管理**：支持预热、健康检查、连接复用、超时控制
- **性能监控**：QPS 统计、延迟百分位数、Prometheus 格式输出

### 1.2 架构设计

```
┌─────────────────────────────────────────────────────────┐
│                      客户端                              │
│                  MySQL Client                           │
└─────────────────────┬───────────────────────────────────┘
                      │ MySQL Protocol
                      ▼
┌─────────────────────────────────────────────────────────┐
│                   Epoll Server                          │
│              (Reactor 模型, 非阻塞 IO)                   │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│                MySQL Parser                             │
│         (协议解析, SQL 类型识别)                         │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│               Connection Pool                           │
│     (连接池管理, 健康检查, 负载均衡)                      │
└─────────────────────┬───────────────────────────────────┘
                      │ MySQL Protocol
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    MySQL Server                        │
└─────────────────────────────────────────────────────────┘
```

---

## 二、核心面试问题

### 2.1 epoll 与 select 的区别

| 特性 | select | epoll |
|------|--------|-------|
| **最大 FD 限制** | FD_SETSIZE (通常 1024) | 无限制，受内存影响 |
| **时间复杂度** | O(n) 轮询所有 FD | O(1) 事件回调 |
| **FD 集合操作** | 每次调用传入/传出 | epoll_ctl 动态管理 |
| **触发模式** | 水平触发 (LT) | LT + 边缘触发 (ET) |
| **适用场景** | FD 较少 | FD 较多，频繁修改 |

**代码示例 - epoll 边缘触发：**
```cpp
// 创建 epoll 实例
int epoll_fd = epoll_create1(EPOLL_CLOEXEC);

// 注册 socket 到 epoll
struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET;  // 边缘触发
ev.data.fd = listen_fd;
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

// 事件循环
while (true) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; ++i) {
        // 处理事件
    }
}
```

### 2.2 Reactor 模型原理

Reactor 模型是一种高性能 IO 处理模式：

1. **单线程事件循环**：使用 epoll 监听 IO 事件
2. **事件分发**：IO 就绪时调用注册的回调函数
3. **非阻塞 IO**：避免线程阻塞等待 IO

**代码示例：**
```cpp
class EventLoop {
public:
    void loop() {
        while (!quit_) {
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
            for (int i = 0; i < n; ++i) {
                // 获取 Channel 并调用回调
                Channel* ch = static_cast<Channel*>(events[i].data.ptr);
                ch->handleEvent(events[i].events);
            }
        }
    }
};
```

**Reactor vs Proactor：**
- Reactor：同步等待 IO，IO 完成后通知
- Proactor：异步 IO，完成后直接回调

### 2.3 连接池线程安全实现

**生产者-消费者模式：**
```cpp
ConnectionPtr ConnectionPool::getConnection(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 尝试从空闲队列获取
    if (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        if (conn->isConnected()) {
            return conn;
        }
        destroyConnection(conn);
    }
    
    // 尝试创建新连接
    if (total_connections_ < max_connections_) {
        return createConnection();
    }
    
    // 等待空闲连接
    cv_.wait_for(lock, timeout);
    return nullptr;
}

void ConnectionPool::returnConnection(ConnectionPtr conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (conn->isConnected()) {
        idle_queue_.push(conn);
        cv_.notify_one();  // 通知等待者
    }
}
```

**关键点：**
- `std::unique_lock` + `std::condition_variable` 实现等待/通知
- `std::atomic` 实现无锁计数器
- RAII 锁管理避免死锁

### 2.4 连接健康检查机制

```cpp
void ConnectionPool::healthCheck() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<ConnectionPtr> valid_queue;
    
    while (!idle_queue_.empty()) {
        auto conn = idle_queue_.front();
        idle_queue_.pop();
        
        // 三种检测方式
        // 1. socket 有效性检测
        if (!conn->isConnected()) {
            destroyConnection(conn);
            continue;
        }
        
        // 2. TCP 心跳检测
        if (!conn->ping()) {
            destroyConnection(conn);
            continue;
        }
        
        // 3. 空闲超时检测
        auto idle_time = std::chrono::steady_clock::now() - conn->lastActiveTime();
        if (idle_time > max_idle_time_) {
            destroyConnection(conn);
            continue;
        }
        
        valid_queue.push(conn);
    }
    
    idle_queue_ = std::move(valid_queue);
}
```

### 2.5 读写分离实现

```cpp
MySQLParser::SQLInfo info = MySQLParser::parseSQL(sql);

std::shared_ptr<ConnectionPool> pool;
if (isReadQuery(info)) {
    // 读请求路由到只读池
    pool = PoolManager::instance().getPool("read_pool");
} else {
    // 写请求路由到主库
    pool = PoolManager::instance().getPool("write_pool");
}
```

### 2.6 性能瓶颈定位

```cpp
PerformanceAnalyzer::BottleneckReport analyze() {
    auto metrics = getCurrentMetrics();
    BottleneckReport report;
    
    // CPU 瓶颈
    if (metrics.cpu_usage > 80) {
        report.type = Type::CPU_BOUND;
        report.recommendations.push_back("增加工作线程数");
    }
    
    // 网络 IO 瓶颈
    if (metrics.network_throughput_mbps > 900) {  // 接近千兆上限
        report.type = Type::NETWORK_BOUND;
        report.recommendations.push_back("升级网络带宽");
    }
    
    // 数据库瓶颈
    if (metrics.p99_latency_ms > 1000) {
        report.type = Type::DATABASE_BOUND;
        report.recommendations.push_back("检查慢查询，添加索引");
    }
    
    // 连接池瓶颈
    if (metrics.connection_pool_usage > 90) {
        report.type = Type::CONNECTION_POOL_EXHAUSTED;
        report.recommendations.push_back("增加连接池大小");
    }
    
    return report;
}
```

---

## 三、MySQL 协议详解

### 3.1 协议结构

MySQL 数据包结构：
```
┌─────────────┬───────────────┐
│  Length (3) │  Sequence ID  │
├─────────────┴───────────────┤
│         Payload             │
└─────────────────────────────┘
```

**包头：**
- 3 字节：Payload 长度（最大 16MB）
- 1 字节：序列号（用于握手）

### 3.2 LENENC 编码

MySQL 特色的长度编码整数：

| 值范围 | 编码方式 |
|--------|----------|
| < 251 | 1 字节 |
| 251-65535 | 0xFC + 2 字节 |
| 65536-16777215 | 0xFD + 3 字节 |
| > 16777215 | 0xFE + 8 字节 |

```cpp
std::optional<uint64_t> readLenEncInt(const char* data, size_t& offset) {
    uint8_t first = data[offset++];
    
    if (first < 0xfb) return first;  // 1 字节
    
    switch (first) {
        case 0xfc:  // 2 字节
            return read<uint16_t>(data, offset);
        case 0xfd:  // 3 字节
            return read<uint24_t>(data, offset);
        case 0xfe:  // 8 字节
            return read<uint64_t>(data, offset);
        default:
            return std::nullopt;
    }
}
```

### 3.3 协议状态机

```
                    ┌─────────────┐
                    │   HANDSHAKE │
                    └──────┬──────┘
                           │ 收到握手包
                           ▼
                    ┌─────────────┐
         ┌─────────│    AUTH     │─────────┐
         │         └──────┬──────┘         │
         │                │ 认证成功        │ 认证失败
         ▼                ▼                 ▼
  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
  │    CLOSE    │  │    QUERY    │  │    ERROR    │
  └─────────────┘  └──────┬──────┘  └─────────────┘
                          │ 收到结果
                          ▼
                   ┌─────────────┐
                   │   RESULT    │
                   └─────────────┘
```

---

## 三、PostgreSQL 协议详解

### 3.1 协议概述

PostgreSQL 使用基于消息的协议，特点：

| 特性 | MySQL | PostgreSQL |
|------|-------|------------|
| 默认端口 | 3306 | 5432 |
| 常见认证 | `mysql_native_password` / `caching_sha2_password` 等 | SCRAM-SHA-256 / md5 等 |
| 整数编码 | LENENC | 固定 2/4/8 字节 |
| 字符串编码 | 字节码 | 始终 UTF-8 |
| 预处理语句 | `?` 占位符 | `$1, $2...` |

### 3.2 消息格式

PostgreSQL 消息结构：
```
┌────────────┬──────────────────────────┐
│  Length   │       Message Body       │
│  (4字节)  │                          │
├───────────┴──────────────────────────┤
│  第一个字节: Message Type            │
│  后续字节: Payload                   │
└─────────────────────────────────────┘
```

**常用消息类型：**
- `Q`: Simple Query
- `P`: Parse (预处理语句)
- `B`: Bind
- `E`: Execute
- `S`: Sync
- `C`: Close
- `d`: CopyData
- `c`: CopyDone
- `H`: Flush
- `X`: Terminate

### 3.3 SCRAM 认证流程

```cpp
// PostgreSQL SCRAM-SHA-256 认证流程
enum class PGAuthState {
    START,
    SEND_SASL_INITIAL,
    RECV_SASL_CONTINUE,
    SEND_SASL_RESPONSE,
    RECV_SASL_FINAL,
    AUTH_SUCCESS
};

// 消息构建
ByteArray buildSCRAMInitial(const std::string& username, const std::string& channel_binding) {
    ByteArray msg;
    msg.push_back('p');  // SASLInitialResponse
    writeCString(msg, "SCRAM-SHA-256");
    writeInt32(msg, channel_binding.length());
    msg.append(channel_binding);
    writeCString(msg, username);
    return msg;
}
```

### 3.4 预处理语句

PostgreSQL 使用 `$n` 格式的占位符：

```cpp
// 解析预处理语句
struct ParseMessage {
    std::string name;      //  prepared statement name
    std::string query;     //  SQL query with $1, $2, etc.
    
    // 示例: "SELECT * FROM users WHERE id = $1 AND status = $2"
    
    std::vector<std::string> extractParams() {
        std::vector<std::string> params;
        // 提取 $1, $2 等参数
        std::regex regex("\\$\\d+");
        std::smatch match;
        std::string str = query;
        while (std::regex_search(str, match, regex)) {
            params.push_back(match.str());
            str = match.suffix().str();
        }
        return params;
    }
};

// Bind 参数
struct BindMessage {
    std::string portal;      // portal name
    std::vector<ParamValue> params;  // $1, $2, ...
    
    // 参数格式: [length (int32)][data (bytes)]
    // 空值: length = -1
};
```

### 3.5 特殊协议

#### LISTEN/NOTIFY (发布订阅)

```cpp
// LISTEN 消息
ByteArray buildListen(const std::string& channel) {
    ByteArray msg;
    msg.push_back('L');  // Listen
    writeCString(msg, channel);
    return msg;
}

// NOTIFY 消息
ByteArray buildNotify(const std::string& channel, const std::string& payload) {
    ByteArray msg;
    msg.push_back('F');  // Function call (向后兼容)
    // 或使用扩展协议
    return msg;
}

// 收到通知: AsyncNotify 消息
struct AsyncNotify {
    std::string channel;
    std::string payload;  // PostgreSQL 10+ 支持
};
```

#### COPY 协议 (批量导入)

```cpp
// COPY FROM stdin
// 1. 发送 Parse 消息
// 2. 发送 Bind 消息
// 3. 发送 Execute 消息 (COPY FROM)
// 4. 接收 CopyInResponse (ReadyForQuery 之前)
// 5. 发送 CopyData 消息 (多行数据)
// 6. 发送 CopyDone 或 CopyFail
// 7. 接收 CommandComplete + ReadyForQuery

ByteArray buildCopyData(const std::string& row_data) {
    ByteArray msg;
    msg.push_back('d');  // CopyData
    writeInt32(msg, row_data.length() + 4);
    msg.append(row_data);
    msg.push_back('\n');
    return msg;
}
```

### 3.6 协议状态机

```
                    ┌─────────────────┐
                    │   Startup       │
                    │  (发送版本号)    │
                    └────────┬────────┘
                             │ 收到认证请求
                             ▼
                    ┌─────────────────┐
                    │    AUTH         │
                    │  (处理认证)      │
                    └────────┬────────┘
                             │ 认证成功
                             ▼
                    ┌─────────────────┐
              ┌─────│      READY      │─────┐
              │     └─────────────────┘     │
              │           │                 │
              ▼           ▼                 ▼
      ┌───────────┐ ┌───────────┐ ┌───────────┐
      │ SIMPLE Q  │ │  Extended │ │   COPY    │
      │  (直接SQL) │ │  (预处理) │ │  (批量)   │
      └─────┬─────┘ └─────┬─────┘ └─────┬─────┘
            │             │             │
            ▼             ▼             ▼
      ┌───────────┐ ┌───────────┐ ┌───────────┐
      │ RowDesc + │ │ RowDesc + │ │ CopyIn/   │
      │ Rows +    │ │ Rows +    │ │ CopyOut   │
      │ Command   │ │ Command   │ │ Command   │
      └─────┬─────┘ └─────┬─────┘ └─────┬─────┘
            │             │             │
            └─────────────┴─────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │  ReadyForQuery   │
                    │  (可继续请求)    │
                    └─────────────────┘
```

### 3.7 MySQL vs PostgreSQL 协议对比

```cpp
// 统一的抽象层设计
class DBProtocol {
public:
    virtual ~DBProtocol() = default;
    
    // 连接
    virtual ByteArray buildHandshake() = 0;
    virtual AuthResult processAuthResponse(const ByteArray& data) = 0;
    
    // 查询
    virtual ByteArray buildQuery(const std::string& sql) = 0;
    virtual ByteArray buildPrepare(const std::string& name, const std::string& sql) = 0;
    virtual ByteArray buildBind(const std::string& portal, 
                               const std::vector<ParamValue>& params) = 0;
    
    // 解析响应
    virtual ParseResult parseResponse(const ByteArray& data) = 0;
};

// MySQL 实现
class MySQLProtocol : public DBProtocol {
    // MySQL 特有的 LENENC 编码
    ByteArray encodeLenEncInt(uint64_t val);
    ByteArray encodeLenEncString(const std::string& str);
};

// PostgreSQL 实现  
class PGProtocol : public DBProtocol {
    // PostgreSQL 特有的 $n 占位符
    std::vector<std::string> extractParamPlaceholders(const std::string& sql);
    ByteArray encodeInt32(int32_t val);
};
```

---

## 四、性能优化技巧

### 4.1 无锁计数器

```cpp
// 使用 atomic 避免锁
std::atomic<int64_t> counter{0};
counter.fetch_add(1);  // 线程安全

// 无锁直方图
struct Histogram {
    static constexpr int BUCKETS = 11;
    std::atomic<int64_t> buckets[BUCKETS];
    
    void record(double value) {
        for (int i = 0; i < BUCKETS; ++i) {
            if (value <= thresholds[i]) {
                buckets[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
};
```

### 4.2 批量处理减少系统调用

```cpp
// 批量 accept
void EpollServer::handleNewConnection() {
    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) break;
        
        // 批量注册到 epoll（使用 EPOLL_CTL_ADD）
        // 或者使用 EPOLL_CTL_MOD 修改已有事件
        // 减少系统调用次数
    }
}
```

### 4.3 零拷贝思路

```cpp
// 使用 buffer 减少内存拷贝
class Buffer {
public:
    void append(const char* data, size_t len) {
        buffer_.insert(buffer_.end(), data, data + len);
    }
    
    // 使用 readv 进行 scatter-gather IO
    ssize_t writeTo(int fd) {
        iovec iovs[2] = {
            {buffer_.data(), std::min(buffer_.size(), static_cast<size_t>(INT_MAX))},
            {extra_data_, extra_len_}
        };
        return writev(fd, iovs, 2);
    }
};
```

### 4.4 延迟计算

```cpp
// 使用 steady_clock 计算延迟
auto start = std::chrono::steady_clock::now();

// 执行操作
process();

// 计算延迟
auto end = std::chrono::steady_clock::now();
auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

// 避免 time_t 的精度问题
// 避免 system_clock 的时钟调整问题
```

---

## 五、常见场景题

### 5.1 如何实现连接池的动态扩容缩容？

```cpp
void ConnectionPool::adjustPoolSize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t current = total_connections_;
    size_t active = busy_connections_;
    
    // 扩容条件：忙碌连接 > 80% 且总连接 < 最大
    if (active > current * 0.8 && current < max_connections_) {
        expandPool();
    }
    
    // 缩容条件：空闲连接 > 最小 且 空闲时间过长
    size_t idle = idle_connections_;
    if (idle > min_connections_ && shouldShrink()) {
        shrinkPool();
    }
}
```

### 5.2 如何处理连接泄漏？

```cpp
// 设置连接最大使用时间
class Connection : public enable_shared_from_this<Connection> {
public:
    bool isExpired() const {
        auto lifetime = std::chrono::steady_clock::now() - created_time_;
        return lifetime > max_lifetime_;
    }
    
    bool isIdleExpired() const {
        auto idle = std::chrono::steady_clock::now() - last_active_;
        return idle > max_idle_time_;
    }
};

// 健康检查时淘汰过期连接
if (conn->isExpired() || conn->isIdleExpired()) {
    destroyConnection(conn);
}
```

### 5.3 如何实现慢查询熔断？

```cpp
class CircuitBreaker {
public:
    enum class State { CLOSED, OPEN, HALF_OPEN };
    
    void recordSuccess() {
        success_count_++;
        if (state_ == State::HALF_OPEN && success_count_ > threshold_) {
            state_ = State::CLOSED;
        }
    }
    
    void recordFailure() {
        failure_count_++;
        if (failure_count_ > threshold_) {
            state_ = State::OPEN;
            open_time_ = std::chrono::steady_clock::now();
        }
    }
    
    bool allowRequest() {
        if (state_ == State::CLOSED) return true;
        
        if (state_ == State::OPEN) {
            if (std::chrono::steady_clock::now() - open_time_ > reset_timeout_) {
                state_ = State::HALF_OPEN;
                return true;
            }
            return false;
        }
        
        return true;  // HALF_OPEN
    }
};
```

### 5.4 如何实现连接池的分片？

```cpp
class ShardedPoolManager {
    std::vector<std::shared_ptr<ConnectionPool>> pools_;
    
    // 一致性哈希分片
    size_t getShard(const std::string& key) {
        uint32_t hash = crc32(key);
        return hash % pools_.size();
    }
    
    ConnectionPtr getConnection(const std::string& key) {
        size_t shard = getShard(key);
        return pools_[shard]->getConnection();
    }
};
```

### 5.5 如何实现优雅关闭？

```cpp
class Server {
public:
    void shutdown() {
        LOG_INFO("Starting graceful shutdown...");
        
        // 1. 停止接收新连接
        accepting_.store(false);
        
        // 2. 通知所有连接关闭
        for (auto& conn : connections_) {
            conn->shutdown();  // 发送 FIN，等待对方关闭
        }
        
        // 3. 等待现有请求完成
        auto deadline = std::chrono::steady_clock::now() + timeout_;
        while (!connections_.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // 4. 强制关闭剩余连接
        for (auto& conn : connections_) {
            conn->forceClose();
        }
        
        // 5. 关闭连接池
        PoolManager::instance().shutdownAll();
        
        LOG_INFO("Graceful shutdown completed");
    }
};
```

---

## 六、扩展问题

### 6.1 eBPF 在网络编程中的应用

```cpp
// 使用 eBPF 追踪网络延迟（示例思路）
// bpftrace 脚本
/*
BEGIN {
    @start[pid] = nsecs;
}

tracepoint:net:netif_rx {
    @latency = (nsecs - @start[pid]) / 1000;
}

END {
    print(@latency);
}
*/

// 应用层获取 eBPF 数据
// 通过 maps 与 eBPF 程序通信
```

### 6.2 容器网络与连接池

```cpp
// Docker 网络模式下的连接池配置
struct PoolConfig {
    int health_check_interval_ms = 5000;  // 容器环境建议缩短
    int connection_timeout_ms = 3000;     // 网络延迟可能更高
    bool enable_reconnect = true;         // 容器重启后自动重连
};
```

### 6.3 分布式追踪

```cpp
// 简化实现：请求追踪
struct TraceContext {
    std::string trace_id;
    std::string span_id;
    std::chrono::steady_clock::time_point start;
};

TraceContext extractTrace(const char* data, size_t len) {
    // 从 MySQL 包中提取追踪信息
    // 或使用专门的追踪协议
}
```

---

## 七、面试话术建议

### 7.1 项目介绍

> "这是一个基于 C++20 的高性能数据库连接池代理中间件，核心是 epoll 实现的 Reactor 模型，单线程事件循环可以支持万级并发连接。同时支持 MySQL 和 PostgreSQL 两种数据库协议，通过统一抽象层设计实现协议无关的连接池管理。连接池采用生产者-消费者模式，通过条件变量实现高效的线程同步，支持连接预热、健康检查、动态扩容缩容等特性。"

### 7.2 技术难点

> "最大的难点是边缘触发模式下处理半包问题。边缘触发只会通知一次，所以必须循环读取直到 EAGAIN。同时还要处理粘包问题，需要按照协议包的包头长度来切割完整包。针对不同数据库协议的差异（如 MySQL 的 LENENC 编码 vs PostgreSQL 的固定长度编码），需要设计统一的抽象接口。"

### 7.3 性能优化

> "我通过几个方面优化性能：1) 使用 epoll 边缘触发减少系统调用；2) 使用无锁原子计数器统计 QPS；3) 使用滑动窗口计算延迟百分位数；4) 使用 buffer 减少内存拷贝；5) 使用 TCP_NODELAY 减少延迟。PostgreSQL 还支持 binary 协议模式，可以进一步减少数据类型转换开销。"

### 7.4 MySQL vs PostgreSQL 选择

> "选择 MySQL 还是 PostgreSQL 取决于业务场景：MySQL 在互联网场景下更常用，读多写少时性能优秀，协议简单高效；PostgreSQL 则在企业级应用中更占优势，支持更丰富的数据类型（JSON、数组、范围类型），MVCC 实现更完善，适合复杂查询和事务一致性要求高的场景。"

---

*文档生成时间: 2026-05-08*
