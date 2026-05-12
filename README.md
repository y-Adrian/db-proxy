# DB-Proxy: 高性能数据库连接池代理中间件

基于 C++20 的数据库连接池与 TCP 代理组件库；主程序 `db-proxy` 对客户端提供 **MySQL / PostgreSQL 原生线协议** 的透明端口转发，并与连接池集成以做预热、健康检查与**并发会话数**约束。

产品与技术演进路线（四阶段、里程碑粒度）见 **[ROADMAP.md](ROADMAP.md)**。  
新人从零上手与推荐阅读顺序见 **[docs/LEARNING.md](docs/LEARNING.md)**。

---

## 主程序 `db-proxy` 在做什么（与文档其它章节的关系）

| 环节 | 实际实现 |
|------|----------|
| 客户端接入 | Linux 下 **epoll（ET）+ 非阻塞 accept**；非 Linux 下为 **select** 回退。`[server] max_connections` / `backlog` 当前**未**接入监听路径（见下方「配置项」）。 |
| 每客户端会话 | 固定大小 **工作线程池**：从 `PoolManager` **借出一条池内连接**，对该连接执行 `COM_QUIT` / `Terminate` 后，再与后端建立**裸 TCP**，在客户端与该 TCP 之间做 **阻塞 `poll` + `recv`/`send` 双向字节透传**；会话结束后 **重新握手** 并归还连接池。 |
| 「连接复用」含义 | **不是**把「已握手的数据库连接」直接转给多个客户端复用（线协议不允许）。池化用于：**限制同时透传的会话数**（与 `[pool] max_connections` 一致）、**预热与健康检查**；每条客户端会话在后端仍为**独立线协议会话**（先裸 TCP再完整握手）。 |
| 协议解析 | 主路径**不做** SQL/报文级解析；`include/protocol/`、`mysql_relay` 等供 **examples / dbcli / 测试** 或后续扩展使用。 |
| 监控 | `[monitor] enable` 控制统计日志与 `PerformanceAnalyzer` 快照；`metrics_interval_ms` 控制周期；`metrics_port>0` 时提供 **HTTP `GET /metrics`**（Prometheus 文本）；`slow_query_threshold_ms` / `enable_query_logging` 作用于**代理会话时长**（非 SQL）。 |

以下「核心价值」中关于**应用直连连接池**（不经代理字节透传）的描述，仍适用于 `examples`、`dbcli`、`ConnectionPool` 等库用法。

---

## 核心价值

### 问题

数据库连接是稀缺资源，高并发场景下频繁创建销毁连接会造成：
- 连接建立耗时（TCP 三次握手 + 数据库认证）
- 数据库服务器连接数爆满
- 内存分配回收抖动

### 方法

**连接池组件 + 主程序代理架构**

```
Client ──线协议──► DB-Proxy (accept: epoll/select)
                      │
                      ├─► 工作线程: 池化槽位 + 裸 TCP 透传 ──► MySQL / PostgreSQL
                      │
                      └─► 后台: 池预热 / 健康检查 / 日志统计
```

### 解决（组件能力）

| 技术点 | 实现 |
|--------|------|
| 连接池（库） | 线程安全获取/归还、上限与等待超时、空闲剔除 |
| 预热与健康 | 启动 `warmup`、定时 `ping`、失效连接剔除与补充 |
| 代理主路径 | **字节级透明**转发整条 TCP 会话（含握手与业务流量） |
| 协议实现（库） | 纯 C++ MySQL/PG 客户端侧协议，无 libmysqlclient（供示例与工具） |
| 监听侧 IO | Linux：**epoll ET + 非阻塞**；其它平台：**select** 回退 |

### 效果（设计目标 / 直连池场景）

- 在 **应用代码直连 `ConnectionPool`** 的场景下，可显著减少反复握手带来的开销（具体数值取决于硬件与负载）。
- 主程序路径侧重 **多会话并发下的槽位与线程模型**；若需「单 TCP 多查询复用」请使用库式连接池而非代理字节流本身。

---

## 技术架构

### 高频短查询与连接开销（直连池）

**问题**：若业务为「每条 SQL 新建 TCP + 握手」，RTT 与认证占比高。

**方法**：使用本仓库提供的 `ConnectionPool` 在进程内持有已认证连接。

**解决**：业务线程 `getConnection` / `returnConnection` 复用同一条后端连接发多条语句。

**效果**：视环境而定；代理主路径不替代该用法。

---

### 多协议与统一出口

**问题**：不同业务连 MySQL 或 PostgreSQL，运维上希望统一端口与配置。

**方法**：`db-proxy` 按配置选择后端线协议，对客户端保持 **与直连数据库相同的 TCP 协议**。

**解决**：应用仍使用现有 **MySQL / PostgreSQL 驱动**，将连接串指向代理监听地址即可。

**效果**：同一代理进程可指向 MySQL 或 PG 后端（由 `protocol` 配置决定）。

---

### 并发模型

**问题**：高并发下 accept 与业务处理如何分工。

**方法**：监听与 accept 在 **Reactor 单线程**（`EpollServer::start`）；每个新连接 `takeConnection` 后交给 **有限个工作线程**，在线程内对单会话做 **阻塞式双向中继**。

**解决**：避免在 epoll 线程内执行阻塞 `recv`/`send`；并发会话数受 **工作线程数** 与 **连接池上限** 共同影响。

**效果**：结构清晰；极端高并发时需调大 `[pool] max_connections` 与 `worker_threads` 并评估线程与 FD 资源。

---

### 连接生命周期

**问题**：应用异常时未归还池连接会导致池耗尽。

**方法**：池内 `returnConnection` / `removeConnection` 与超时等待；代理路径在会话线程结束时 **归还并重连** 槽位。

**解决**：会话与池槽位一一绑定在借还周期内，避免泄漏。

---

## 技术栈

| 领域 | 技术 |
|------|------|
| 语言 | C++20 |
| 网络 | Linux: **epoll ET + 非阻塞 accept**；会话内阻塞 `poll` 透传；非 Linux: **select** 回退 |
| 数据库 | MySQL / PostgreSQL **线协议**（代理透传 + 库内协议栈） |
| 并发 | 工作线程池、`std::jthread` 后台健康/统计、原子与互斥 |
| 监控 | `Metrics`（Prometheus 文本 + 可选 `/metrics` HTTP）、`Statistics` 会话级 QPS、周期池 Gauge 同步 |

## 核心模块

```
db-proxy/
├── conf/              # 按后端拆分的示例 INI（MySQL / PostgreSQL）
├── include/
│   ├── core/          # 配置、日志
│   ├── network/       # epoll/select 监听与 TcpConnection
│   ├── protocol/      # 透明中继、MySQL 包/中继（主程序透传；解析供示例与扩展）
│   ├── pool/          # 连接池与后端连接封装
│   └── monitor/       # Metrics、Statistics、HTTP /metrics、PerformanceAnalyzer
└── src/
    ├── main.cpp       # 主程序：池化槽位 + 工作线程 + 透明会话中继
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

# 编译完成后，所有二进制在 build/ 下；示例配置在仓库根 conf/：
#   conf/proxy.mysql.conf          MySQL 后端（代理默认 6033）
#   conf/proxy.postgresql.conf     PostgreSQL 后端（代理默认 6432，与 MySQL 可同时跑）
#   build/db-proxy                 主程序（池化 + 透明线协议代理）
#   build/examples                 MySQL 场景化用例
#   build/examples_pg              PostgreSQL 场景化用例（可选 OpenSSL）
#   build/test_pool                连接池集成测试（需本地 MySQL）
#   build/test_pooled_session_relay  中继单元测试（仅本机，无需数据库）
#   build/test_monitor_integration   监控 /metrics、Statistics、配置解析集成测试（仅本机）
#   build/bench_pool               性能测试
#   build/stress_test              压力测试
#   build/dbcli                    CLI 工具
#   build/diagnostics_demo         诊断模块示例
```

### 运行主程序

```bash
cd build
./db-proxy
# 无 -c 时自动选用存在的 conf/proxy.mysql.conf（先试 ../conf/ 再试 conf/，适配 cwd 为 build/ 或仓库根）。
# 显式切换后端：
# ./db-proxy -c ../conf/proxy.mysql.conf
# ./db-proxy -c ../conf/proxy.postgresql.conf
```

### 智能诊断示例（Ollama）

`build/diagnostics_demo` 会组装 `Statistics` / `Metrics` 快照，经 **`DiagnosticEngine`** 调用本地 **Ollama**（`/api/chat`，默认模型 **`qwen2.5-coder:7b`**），生成控制台报告并写入 `build/diagnostics/reports/`（Markdown / JSON）。

```bash
cd build
# 需本机已启动 Ollama 且已拉取模型，例如：ollama pull qwen2.5-coder:7b
./diagnostics_demo mock
./diagnostics_demo ollama
./diagnostics_demo ollama --model qwen2.5-coder:7b --timeout 180

# 可选环境变量（命令行 --url / --model 优先覆盖）
export OLLAMA_HOST=127.0.0.1:11434          # 或完整 URL，如 http://192.168.1.10:11434
export OLLAMA_MODEL=qwen2.5-coder:7b
./diagnostics_demo ollama
```

程序启动时会初始化 **`Logger`**，`OllamaProvider` 等模块的 `LOG_*` 会出现在标准输出，便于排查请求与解析问题。

### db-proxy 使用方式（MySQL / PostgreSQL）

**在做什么**：每个 `db-proxy` 进程根据所选 INI **只连一种后端线协议**（`[database.primary]` 里的 `protocol`）。客户端仍使用**普通 MySQL 或 PostgreSQL 驱动**，把连接目标改成 **代理监听地址与端口** 即可；代理与后端之间由池完成握手并保持连接。

| 步骤 | MySQL | PostgreSQL |
|------|--------|--------------|
| 1. 构建 | `cmake .. && make`；无需 OpenSSL 也可连 `mysql_native_password` / 部分 `caching_sha2` 场景 | 需 **CMake 找到 OpenSSL**（否则不会编译 PG 池）；`pg_hba` 常见为 **SCRAM-SHA-256** |
| 2. 选配置 | `conf/proxy.mysql.conf`（代理默认 **6033** → 后端 **3306**） | `conf/proxy.postgresql.conf`（代理 **6432** → 后端 **5432**，与 MySQL 代理可同时起两个进程） |
| 3. 改连接信息 | 编辑对应 conf 里 `[database.primary]` 的 `host` / `port` / `user` / `password` / `database` | 同上；**本机 Homebrew** 可不写 `user`，程序用环境变量 **`USER`**；**Docker PG** 一般在 conf 里写 `user = postgres` |
| 4. 启动 | 仓库根：`./build/db-proxy -c conf/proxy.mysql.conf`；在 `build/`：`./db-proxy -c ../conf/proxy.mysql.conf`（无 `-c` 时会自动查找 `conf/proxy.mysql.conf` 或 `../conf/proxy.mysql.conf`） | 同上，把文件名换成 `proxy.postgresql.conf` |
| 5. 应用连接串 | 主机 = 跑代理的机器，端口 = **6033**，库名/用户/密码与直连后端一致（由池代为连后端） | 主机同上，端口 = **6432**，其余与直连 PG 一致 |

**说明**：`[server] port` 为代理对外端口；`[database.primary] port` 为**真实数据库**端口。各配置键说明见下文「配置文件 `conf/` 与主程序」；示例与行内注释见 `conf/proxy.mysql.conf`、`conf/proxy.postgresql.conf`。

### 自动化测试

```bash
cd build
# 仅跑中继相关单测（不依赖 MySQL）
./test_pooled_session_relay
ctest -R PooledSessionRelay --output-on-failure

# 连接池集成测试 `test_pool`：默认跳过；需本机数据库且显式开启其一
#   MySQL：  DBPROXY_TEST_MYSQL=1 ./test_pool
#   PostgreSQL（需 OpenSSL 构建）：  DBPROXY_TEST_PG=1 PGHOST=127.0.0.1 PGPORT=5432 PGUSER=… ./test_pool
# 或一键脚本（含建表/编译/场景用例/同方式 test_pool）：
#   ./test_with_mysql.sh
#   ./test_with_pg.sh
# 上述脚本在跑完 `examples` / `examples_pg` 后，会以与 MySQL 相同方式执行 `DBPROXY_TEST_*=1 ./test_pool`。
# 手动池测示例（在已启动对应数据库的前提下）：
#   DBPROXY_TEST_MYSQL=1 ./test_pool
#   DBPROXY_TEST_PG=1 ./test_pool   # 使用 PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE
ctest -R PoolTest --output-on-failure

# 监控模块集成测试（HTTP /metrics、Statistics、[monitor] 配置）
./test_monitor_integration
ctest -R MonitorIntegration --output-on-failure

# 全部 CTest（`test_pool` 默认跳过；若未设置 DBPROXY_TEST_* 则仍为通过）
ctest --output-on-failure
```

### 配置文件 `conf/` 与主程序

| 文件 | 说明 |
|------|------|
| `conf/proxy.mysql.conf` | MySQL 后端：`protocol=mysql`，代理端口 **6033**（默认 `-c` 目标，工作目录为仓库根时）。 |
| `conf/proxy.postgresql.conf` | PostgreSQL 后端：`protocol=postgresql`，代理端口 **6432**，日志默认 `./logs/proxy-pg.log`。**不写 `user` 时用环境变量 `USER`**（适配 Homebrew）；Docker 常见需 `user = postgres`。 |
| `proxy.conf`（仓库根） | 仅占位说明；**请使用 `conf/` 下对应文件**，避免在两种后端间来回改同一配置。 |

| 配置段 / 键 | 说明 |
|-------------|------|
| `[server] max_connections`、`backlog` | 已由 `loadConfig` 解析，**监听与 accept 路径尚未使用**（见 `EpollServer::listen`）。 |
| `[pool] max_connections` | **已使用**：与主程序并发透传会话数一致（池借出即占用槽位）。 |
| `[pool] connection_timeout_ms` | **已使用**：`getConnection` 等待空闲槽位的超时。 |
| `[monitor]`* | `enable`：总开关；`metrics_interval_ms`：统计日志与池 Gauge 同步周期；`metrics_host` / `metrics_port`：`metrics_port>0` 时监听并暴露 `GET /metrics`；`slow_query_threshold_ms` / `enable_query_logging`：按**会话持续时间**记慢会话与会话日志（透传路径无 SQL）。 |
| `[database] protocol` | `mysql`（默认）或 `postgresql` / `postgres` / `pg`。PostgreSQL 池与 SCRAM 等认证依赖 **OpenSSL** 构建；MySQL `caching_sha2_password` 的 RSA 完整认证同样依赖 OpenSSL。 |

---

## 数据库测试

项目提供两个测试脚本，均支持 **local**（连接本地数据库）和 **docker**（容器化）两种模式，编译产物统一输出到 `build/` 目录。

### PostgreSQL 测试

构建阶段需 **CMake 找到 OpenSSL** 且未关闭 `DBPROXY_ENABLE_POSTGRES`，才会生成 `examples_pg` 与池内的 `PostgreSQLConnection`；运行主程序代理 PG 时使用 **`conf/proxy.postgresql.conf`**（或自行复制后改 `protocol` 与连接信息）。认证侧实现支持 trust、明文密码、MD5 与 **SCRAM-SHA-256**（见 `src/pool/postgresql_connection.cpp`）。

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

与 MySQL 脚本对称：成功检测数据库并编译后，会在 `build/` 下依次运行 **`./examples_pg`** 与 **`DBPROXY_TEST_PG=1 ./test_pool`**（后者使用 `PGHOST` / `PGPORT` / `PGUSER` / `PGPASSWORD` / `PGDATABASE` 与 `tests/test_connection_pool.cpp` 中的 PG 池逻辑）。

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

流程与 PG 对称：检测/启动 MySQL → 建表 → 编译 → 运行 **`./examples`**，再运行 **`DBPROXY_TEST_MYSQL=1 ./test_pool`**（固定 `127.0.0.1:3306` / `root` / 空密码 / `test`，与 `tests/test_connection_pool.cpp` 中 MySQL 分支一致）。可选 `--also-pg` 再跑 `examples_pg`。

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
