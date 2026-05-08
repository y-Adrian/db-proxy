# DB-Proxy: 高性能数据库连接池代理中间件

基于 C++20 的高性能数据库连接池/代理中间件，适用于 MySQL 协议，适用于面试展示。

## 项目亮点（面试要点）

### 1. 高性能网络编程
- **epoll LT/ET 模式**：边缘触发模式减少系统调用，提升性能
- **非阻塞 IO**：O_NONBLOCK + 边缘触发，实现真正的异步 IO
- **Reactor 模型**：单线程事件循环，避免锁竞争
- **连接数支持**：万级并发连接

```cpp
// epoll 边缘触发示例
struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET;  // 边缘触发
epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
```

### 2. 连接池设计
- **生产者-消费者模式**：多线程安全的连接管理
- **条件变量 + 互斥锁**：高效线程同步
- **连接预热**：启动时创建最小连接数
- **健康检查**：定时检测连接有效性
- **连接复用**：减少数据库连接开销

```cpp
ConnectionPtr ConnectionPool::getConnection(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    while (true) {
        if (!idle_queue_.empty()) {
            auto conn = idle_queue_.front();
            idle_queue_.pop();
            if (conn->isConnected() && conn->ping()) {
                return conn;
            }
            destroyConnection(conn);
        }
        
        if (total_connections_.load() < max_connections_) {
            return createConnection();
        }
        
        cv_.wait_for(lock, remaining);
    }
}
```

### 3. MySQL 协议解析
- **协议状态机**：处理握手、认证、查询流程
- **LENENC 编码**：MySQL 特色的长度编码整数
- **SQL 类型识别**：读写分离的基础

### 4. 性能监控
- **无锁计数器**：原子变量减少锁竞争
- **滑动窗口**：计算 QPS/TPS
- **P50/P90/P99/P999**：延迟百分位数
- **Prometheus 格式**：可观测性支持

```cpp
// 原子计数器
std::atomic<int64_t> counter{0};
counter.fetch_add(1);

// 无锁直方图
for (int i = 0; i < BUCKETS; ++i) {
    if (value <= buckets[i].le) {
        buckets[i].count.fetch_add(1);
    }
}
```

## 技术栈

| 领域 | 技术 |
|------|------|
| 语言 | C++20 |
| 网络 | epoll, 非阻塞 IO, Reactor 模型 |
| 数据库 | MySQL 协议, 连接池 |
| 并发 | 多线程, 原子操作, 条件变量 |
| 监控 | Prometheus 格式, 滑动窗口统计 |

## 核心模块

```
db-proxy/
├── include/
│   ├── core/          # 核心配置和日志
│   ├── network/       # epoll 网络层
│   ├── protocol/      # MySQL 协议解析
│   ├── pool/          # 连接池管理
│   └── monitor/        # 性能监控
└── src/
    ├── main.cpp        # 主程序入口
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

## 面试问答要点

### Q: epoll 和 select 的区别？
- select 有 FD_SETSIZE 限制（通常 1024），epoll 没有
- select 需要每次传入所有 fd，epoll 通过 epoll_ctl 动态添加删除
- select 是轮询 O(n)，epoll 是回调 O(1)
- epoll 支持边缘触发（ET）和水平触发（LT）

### Q: 什么是 Reactor 模型？
- 单线程事件循环，监听 IO 事件
- 事件就绪时调用回调处理
- 避免多线程同步开销
- 适用于 IO 密集型场景

### Q: 连接池如何实现线程安全？
- 互斥锁保护共享状态
- 条件变量实现等待/通知
- 原子变量用于计数器
- unique_lock/lock_guard 管理锁生命周期

### Q: 如何检测连接失效？
- 心跳检测：定期发送 ping
- 超时检测：检查空闲时间
- 读写检测：尝试发送测试语句
- 错误检测：处理 IO 错误

### Q: 如何实现读写分离？
- 解析 SQL 类型（SELECT vs INSERT/UPDATE/DELETE）
- 维护只读池和读写池
- 根据 SQL 类型路由到对应池
- 主从延迟处理（可选）

### Q: 性能瓶颈如何定位？
- CPU 使用率：top/htop
- 网络 IO：netstat/ss
- 数据库延迟：慢查询日志
- 连接池状态：使用率/等待时间
- 内存使用：pmap/vmmap

## 扩展方向

1. **读写分离**：主从架构
2. **分库分表**：数据分片
3. **连接池分片**：多池路由
4. **SQL 防火墙**：注入检测
5. **缓存层**：Redis 集成
6. **容器化**：Docker 部署

## 参考资料

- [Linux IO 多路复用详解](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [MySQL 协议文档](https://dev.mysql.com/doc/internals/en/client-server-protocol.html)
- [C++ 并发编程实战](https://en.cppreference.com/w/cpp/thread)
