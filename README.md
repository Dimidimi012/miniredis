# miniredis

一个用标准 C11 编写的、兼容 Redis 协议（RESP）的轻量级内存键值数据库。
它实现了一个真实的 TCP 服务器，可以把它当作一个可运行的小型缓存使用

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


### 已支持命令

| 类别 | 命令 |
|---|---|
| 连接 | `PING` `ECHO` `QUIT` `SELECT` |
| 字符串 | `SET`（含 `EX/PX/EXAT/PXAT/NX/XX`）`GET` `MGET` `MSET` `SETNX` `SETEX` `GETSET` `APPEND` `STRLEN` `GETRANGE` `SETRANGE` `RENAME` `INCR` `DECR` |
| 键 | `DEL` `EXISTS` `TYPE` `KEYS`（glob 匹配） |
| 过期 | `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` |
| 列表 | `LPUSH` `RPUSH` `LPUSHX` `RPUSHX` `LPOP` `RPOP` `LLEN` `LRANGE` `LINDEX` `LSET` `LTRIM` `LREM` `LINSERT` |
| 哈希 | `HSET` `HMSET` `HGET` `HMGET` `HGETALL` `HKEYS` `HVALS` `HLEN` `HEXISTS` `HDEL` `HSETNX` `HINCRBY` `HINCRBYFLOAT` |
| 集合 | `SADD` `SREM` `SISMEMBER` `SCARD` `SMEMBERS` `SPOP` `SINTER` `SUNION` `SDIFF` |
| 有序集合 | `ZADD`（含 `NX/XX/CH/INCR`）`ZCARD` `ZSCORE` `ZREM` `ZRANGE` `ZREVRANGE` `ZRANGEBYSCORE`（含 `WITHSCORES`/`LIMIT`）`ZRANK` `ZREVRANK` `ZINCRBY` `ZCOUNT` |
| 服务器 | `INFO` `DBSIZE` `FLUSHALL` `COMMAND` `SAVE` `BGSAVE` `REWRITEAOF` `BGREWRITEAOF` |

未实现的命令会返回标准错误：`-ERR unknown command '...'`。

---

依赖：`cc`（GCC/Clang）与 `make`。目标平台为 **Linux / macOS**（Windows 请用 WSL 或
MSYS2/MinGW）。

```bash
make            # 构建 ./miniredis
make test       # 单元测试 + 端到端测试
./miniredis --help
./miniredis --bind 127.0.0.1 --port 6379          # 默认 epoll（Linux）
./miniredis --io select --port 6380              # 显式切换 select 事件循环
```

用官方客户端验证：

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
- **AOF 重写**：`REWRITEAOF`（同步）/ `BGREWRITEAOF`（fork 子进程）把当前状态序列化为
  紧凑命令流（按类型批量 `SET/RPUSH/HSET/SADD/ZADD` + `PEXPIREAT`），写入临时文件后原子
  `rename` 替换原 AOF——日志不再无限增长。重写期间的新写入会进入内存缓冲，子进程换文件后
  由父进程（SIGCHLD 回收时）重开 fd 并补写，一条不丢。
- **RDB**：自定义二进制快照格式（格式见 `src/rdb.h`），全部多字节整数大端序，
  保存到 `<path>.tmp` 后原子 `rename`，绝无半写文件。`BGSAVE` 通过 `fork()` 获得
  一致视图，父进程不阻塞。
- 启动顺序：先加载 RDB（若存在），再重放 AOF（若存在）——与 Redis 的恢复语义一致。

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

## 特色

- 粘包/半包处理：每个客户端维护一个输入缓冲 `dynbuf in`，`resp_parse_command`
  在缓冲不足时返回"需要更多数据"而不阻塞或误判；一次 `recv` 后循环解析出尽可能多的
  完整命令。
- 内存所有权清晰：`dict` 复制键并接管值（`robj*`），`command` 拥有其 `argv`；
  替换/删除时由调用方释放旧值，避免悬挂指针与泄漏。
- 惰性过期：读路径统一走 `lookup_key()`，命中即检查 `expire_at`，过期则删除并
  返回不存在，保证对外语义一致。

---



