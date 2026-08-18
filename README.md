# miniredis

一个用标准 C11 编写的、**兼容 Redis 协议（RESP）的轻量级内存键值数据库**。
它实现了一个真实的 TCP 服务器，你可以直接用官方 `redis-cli` 连上去操作，用
`redis-benchmark` 压测，甚至把它当作一个可运行的小型缓存使用——不是玩具，而是一个
端到端可用、可验证的系统级项目。

```text
$ make
$ ./miniredis --port 6379

# 另一个终端
$ redis-cli -p 6379
127.0.0.1:6379> SET user:1 "alice"
OK
127.0.0.1:6379> GET user:1
"alice"
127.0.0.1:6379> INCR visits
(integer) 1
127.0.0.1:6379> EXPIRE user:1 60
(integer) 1
127.0.0.1:6379> TTL user:1
(integer) 60
```

---

## 特性

- **RESP 协议**：完整解析客户端请求（数组 / 批量字符串 / 简单字符串），正确处理
  TCP 粘包/半包（增量解析，等待完整帧后再执行）。
- **双事件循环**：Linux 下默认 `epoll`（`--io select|epoll` 可切换），select 为跨平台回退。
- **手写数据结构**：哈希表（链地址法 + 0.75 扩容 + **SipHash-2-4 键控哈希 + /dev/urandom
  随机种子**，抗哈希洪水攻击）、双向链表、**带 span 的跳表**（ZSET，支持 O(log n) 排名查询）；
  键值均**二进制安全**。
- **5 种数据类型**：STRING / LIST / HASH / SET / ZSET（全部支持过期）。
- **持久化**：AOF 追加日志（每条写命令落盘，**appendfsync everysec** 定时刷盘、重启重放）
  + RDB 快照（原子写入、`SAVE`/`BGSAVE`、优雅关停自动保存、启动自动加载）。
- **内存防护**：客户端查询输入缓冲 64MB 上限，超限断开连接，防止慢/恶意客户端耗尽内存。
- **过期机制**：`EXPIRE`/`PEXPIRE`/`SET ... EX/PX`，惰性删除（读时判断）+ 精确到毫秒。
- **工程完整**：Makefile + CMake、单元测试 + 端到端测试、`-Wall -Wextra -Wpedantic`、
  README + 架构说明。

### 已支持命令

| 类别 | 命令 |
|---|---|
| 连接 | `PING` `ECHO` `QUIT` `SELECT` |
| 字符串 | `SET`（含 `EX/PX/EXAT/PXAT/NX/XX`）`GET` `INCR` `DECR` |
| 键 | `DEL` `EXISTS` `TYPE` `KEYS`（glob 匹配） |
| 过期 | `EXPIRE` `PEXPIRE` `TTL` `PTTL` |
| 列表 | `LPUSH` `RPUSH` `LPUSHX` `RPUSHX` `LPOP` `RPOP` `LLEN` `LRANGE` `LINDEX` `LSET` `LTRIM` `LREM` `LINSERT` |
| 哈希 | `HSET` `HMSET` `HGET` `HMGET` `HGETALL` `HKEYS` `HVALS` `HLEN` `HEXISTS` `HDEL` `HSETNX` `HINCRBY` `HINCRBYFLOAT` |
| 集合 | `SADD` `SREM` `SISMEMBER` `SCARD` `SMEMBERS` `SPOP` `SINTER` `SUNION` `SDIFF` |
| 有序集合 | `ZADD`（含 `NX/XX/CH/INCR`）`ZCARD` `ZSCORE` `ZREM` `ZRANGE` `ZREVRANGE` `ZRANGEBYSCORE`（含 `WITHSCORES`/`LIMIT`）`ZRANK` `ZREVRANK` `ZINCRBY` `ZCOUNT` |
| 服务器 | `INFO` `DBSIZE` `FLUSHALL` `COMMAND` `SAVE` `BGSAVE` |

未实现的命令会返回标准错误：`-ERR unknown command '...'`。

---

## 快速开始

依赖：`cc`（GCC/Clang）与 `make`。目标平台为 **Linux / macOS**（Windows 请用 WSL 或
MSYS2/MinGW）。

```bash
make            # 构建 ./miniredis
make test       # 单元测试 + 端到端测试
./miniredis --help
./miniredis --bind 127.0.0.1 --port 6379          # 默认 epoll（Linux）
./miniredis --io select --port 6380              # 显式切换 select 事件循环
```

用官方客户端验证（推荐，最能体现"实用"）：

```bash
redis-cli -p 6379 PING          # PONG
redis-cli -p 6379 SET k v       # OK
redis-cli -p 6379 GET k         # "v"
redis-benchmark -p 6379 -n 100000 -c 50 -t set,get
```

没有 `redis-cli` 也可以：`make test` 里的 `tests/test_client` 是一个零依赖的 RESP
客户端，直接对协议做断言。

### 持久化

```bash
# AOF：每次写命令立即追加到日志，重启时重放（崩溃恢复）
./miniredis --aof appendonly.aof &

# RDB：优雅关停（SIGTERM/SIGINT）自动保存快照；也可手动 SAVE / BGSAVE
./miniredis --rdb dump.rdb &

# 两者可同时启用：启动时先加载 RDB，再重放 AOF（AOF 优先，语义与 Redis 一致）
./miniredis --aof appendonly.aof --rdb dump.rdb &

redis-cli -p 6379 SAVE      # 同步写快照
redis-cli -p 6379 BGSAVE    # fork 子进程异步写快照（CoW 一致视图）
```

- **AOF**：写命令在执行前以 RESP 形式追加到文件；`appendfsync everysec`（事件循环每秒
  `fsync` 一次，关停时再 `fsync`）；重启时逐条重放，能恢复 `kill -9` 这类未优雅退出后的
  数据。相对过期时间（`EXPIRE`/`SET EX`）在重放时相对重启时刻重新计算（与无重写的 Redis
  行为一致）。
- **RDB**：自定义二进制快照格式（格式见 `src/rdb.h`），全部多字节整数大端序，
  保存到 `<path>.tmp` 后原子 `rename`，绝无半写文件。`BGSAVE` 通过 `fork()` 获得
  一致视图，父进程不阻塞。
- 启动顺序：先加载 RDB（若存在），再重放 AOF（若存在）——与 Redis 的恢复语义一致。

---

## 架构

```text
                     ┌─────────────────────────────────────┐
  redis-cli / 客户端 ──►│  server.c  单线程事件循环           │
                     │  Linux: epoll（默认）               │
                     │  其他: select（跨平台回退）          │
                     │  accept / 非阻塞 read / 非阻塞 write │
                     └───────────────────┬─────────────────┘
                                         │ 完整命令 (command)
                                         ▼
                     ┌─────────────────────────────────────┐
                     │  resp.c    RESP 增量解析器           │
                     │  处理粘包/半包，产出 argv[]          │
                     └───────────────────┬─────────────────┘
                                         ▼
                     ┌─────────────────────────────────────┐
                     │  db.c      命令分发 + 业务处理        │
                     │  lookup_key 惰性过期 → dict 读写      │
                     └───────────────────┬─────────────────┘
                                         ▼
                     ┌─────────────────────────────────────┐
                     │  dict.c    哈希表 / object.c 值对象   │
                     │  dynbuf.c  动态缓冲（请求/响应/粘包）  │
                     └─────────────────────────────────────┘
```

### 模块划分

| 文件 | 职责 |
|---|---|
| `src/server.c` | TCP 服务器、epoll（Linux）/ select 事件循环、客户端读写缓冲、信号处理 |
| `src/resp.c` | RESP 协议解析（增量、边界安全、上限保护） |
| `src/db.c` | 键值存储、命令分发表、惰性过期、各命令实现 |
| `src/dict.c` | 通用哈希表（`dict_set/get/delete` + 迭代器） |
| `src/object.c` | 值对象 `robj`（二进制安全字符串 + 过期时间戳） |
| `src/dynbuf.c` | 可增长字节缓冲（类似 Redis 的 sds） |
| `src/util.c` | `xmalloc` 系列、日志、毫秒时钟、严格整数解析、glob 匹配 |

### 关键设计点

- **粘包/半包处理**：每个客户端维护一个输入缓冲 `dynbuf in`，`resp_parse_command`
  在缓冲不足时返回"需要更多数据"而不阻塞或误判；一次 `recv` 后循环解析出尽可能多的
  完整命令。
- **内存所有权清晰**：`dict` 复制键并接管值（`robj*`），`command` 拥有其 `argv`；
  替换/删除时由调用方释放旧值，避免悬挂指针与泄漏。
- **惰性过期**：读路径统一走 `lookup_key()`，命中即检查 `expire_at`，过期则删除并
  返回不存在，保证对外语义一致。

---

## 测试

```bash
make test-unit         # test_util / test_dict / test_resp（不依赖网络）
make test-integration  # 启动服务器 + test_client 端到端断言
make test              # 全部
```

测试覆盖：整数解析边界、glob 匹配、**SipHash 已知向量**、哈希表增删改查/二进制键/扩容/
迭代、RESP 完整与残缺帧、一条完整的 `PING/SET/GET/INCR/EXPIRE/TTL/KEYS/未知命令` 链路、
并发 `burst` 压力、**超大请求断连（biginput，>64MB）**、以及 AOF/RDB 两条持久化恢复链路。

---

## 性能说明

实现了两套可切换的事件循环：**epoll**（Linux 默认）与 **select**（跨平台回退），
通过 `--io select|epoll` 切换，方便直接对比基准。`select` 受 `FD_SETSIZE`（默认 1024）
限制；epoll 可支撑数万并发连接。单线程串行处理命令，`redis-benchmark` 下 SET/GET
通常可达数万 ~ 十几万 QPS（视机器而定）。

```bash
./miniredis --io select &   # 分别压测两套循环
redis-benchmark -p 6379 -n 100000 -c 100 -t set,get
```

---

## Roadmap（后续迭代方向）

- [x] `epoll` 事件循环（Linux 默认）+ `select` 跨平台回退
- [x] 更多数据类型：LIST / HASH / SET / ZSET（跳表实现排行榜）
- [x] 持久化：AOF 追加日志 + RDB 快照（SAVE/BGSAVE/崩溃恢复）
- [x] 抗哈希洪水攻击：SipHash-2-4 + `/dev/urandom` 真随机种子
- [ ] AOF 重写（BGREWRITEAOF）与 `appendfsync always` 策略
- [ ] `kqueue` 事件循环（macOS/BSD）
- [ ] 定期过期清理（当前仅惰性删除）+ 主动内存回收
- [ ] 主从复制、`MONITOR`、`PUB/SUB`

---

## 许可

MIT
