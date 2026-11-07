# miniRPC + KV 第一版

2026-11-04。范围为 Linux 单节点远程 KV，复用相邻 miniRPC 的 Reactor 主路径。

```text
kv_client / kv::rpc::Client
    → miniRPC (KVService.Get / Put / Delete / Write)
    → kv_rpc 参数校验和状态映射
    → kv_core (DB / WAL / MemTable / SSTable)
```

## 构建和运行

`KV_ENABLE_RPC` 默认 OFF，纯 KV 不需要 miniRPC 或 Protobuf。开启后要求 Linux，
通过 `find_package(miniRPC CONFIG REQUIRED)` 链接导出目标 `minirpc::minirpc`。
miniRPC 已依赖 Protobuf；本适配层使用同一套编译器与运行库生成业务消息。

从 KV 仓库目录，在 Linux 执行：

```bash
cmake -S ../miniRPC -B ../miniRPC/build -DCMAKE_BUILD_TYPE=Release
cmake --build ../miniRPC/build --parallel
cmake --install ../miniRPC/build --prefix /tmp/minirpc-prefix
cmake -S . -B build-rpc -DKV_ENABLE_RPC=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/tmp/minirpc-prefix
cmake --build build-rpc --parallel
ctest --test-dir build-rpc --output-on-failure
./build-rpc/rpc/kv_server ./rpc_db 127.0.0.1 9000
```

另一个终端：

```bash
./build-rpc/rpc/kv_client 127.0.0.1 9000 put user:1 alice
./build-rpc/rpc/kv_client 127.0.0.1 9000 get user:1
./build-rpc/rpc/kv_client 127.0.0.1 9000 write put user:2 bob delete user:1
```

macOS 开发环境使用已有容器：

```bash
./scripts/run_rpc_checks.sh
```

该脚本要求 Docker 已启动，且已有 `minirpc-linux` 将相邻 `miniRPC` 挂载到 `/work`、
`/work/build` 使用 Linux 工具链。它将 KV 源码复制到容器的独立 `/tmp/kv-rpc-check.*`
目录，安装并链接现有 miniRPC 库，运行 KV/RPC 全部测试，最后打印可直接使用的启动命令。
不创建或替换容器，不覆写宿主 KV 的构建目录。可设置 `BUILD_TYPE=Release`；
`KV_SANITIZERS=address,undefined` 仅插桩 KV/适配层，导入的 miniRPC 库仍使用原构建配置。

## 接口和协议

公共适配 API 在 `rpc/include/kv_rpc/`。链接目标 `kv_rpc` 或别名 `kv::rpc`，
服务端调用 `RegisterService(server, db)`。DB 必须先 Open，注册必须在 Start 前完成，
DB 必须比 server 活得更久；停止时先 `server.Stop()` 等待 handler，再 `db.Close()`。

外层沿用 miniRPC 的 20 字节帧头及现有 Protobuf envelope，业务 `payload` 为
`rpc/proto/kv_service.proto`，版本号为 1。KV 数据使用 `bytes`，保留二进制值。
Get 使用 `GetRequest`；Put/Delete 使用单操作 `WriteRequest`；Write 使用有序多操作请求。
服务端先验证整个请求，之后只调用一次 `DB::Write()`；同批重复 key 按操作顺序生效。
空 Write 是合法 no-op，但仍检查数据库状态。

2026-11-07 的 KV 并发实现由 WriteQueue 选出唯一组首执行 WAL/sequence/MemTable 提交，
关闭 grouping 也保持单写者，因此已移除额外的 `write_mu_`。SkipList 通过原子后继
Release/Acquire 发布不可变节点，已移除 `memtable_mu_`；多个 RPC handler 可以并发调用 DB，
写入串行提交，Get 按固定序号读取。服务停止仍先 drain handler，再由 DB::Close 停止准入并排空写队列。
锁职责见 [README 并发模型](../README.md#并发模型)。

RPC 限制：

| 项目 | 上限/规则 |
| --- | --- |
| 编码后的业务请求 payload | 1.5 MiB，预留 miniRPC envelope 空间 |
| 单 value | 1 MiB，保证成功写入的值可通过 Get 读回 |
| 单批操作 | 4096 |
| key | 非空，不含 tab、newline、NUL，另受请求体上限限制 |
| 操作类型 | 仅 PUT 和 DELETE；Delete 不携带 value |

客户端和服务端均校验边界，未知版本或枚举明确拒绝。本地 DB 中原有超大 value
通过 RPC Get 返回 InvalidArgument，不截断数据。CLI 参数不支持 NUL；需要二进制
数据时使用 C++ Client。

## 返回值与提交状态

`Result.transport_status` 表达 miniRPC 传输/协议结果。只有 transport 成功时，
`Result.status` 才是服务器 KV 结果；`ok()` 同时检查两层。
业务 Reply 独立表达 OK/NotFound/InvalidArgument/IOError/Corruption。

| commit_state | 含义 |
| --- | --- |
| NotApplicable | Get，无写入 |
| NotCommitted | 本地或服务器在调用 DB::Write 前拒绝 |
| Committed | DB::Write 返回成功；空 Write 表示 no-op 完成 |
| Unknown | 调用 DB::Write 后失败，或发起 RPC 后未获得可信成功响应 |

未知状态保守处理：WAL 可能已完整写入；错误也可能发生在 MemTable 发布之后。
超时不会撤销执行中的写入。客户端不自动重放写操作，不提供 exactly-once、持久化去重
或结果查询；miniRPC 帧 request_id 仅用于匹配响应，不是业务幂等 ID。
miniRPC 的连接及发送仍包含阻塞操作，因此传入 timeout 不能作为严格的端到端
耗时上限；本轮超时测试验证的是请求已进入 handler 后等待响应的场景。
若需要确认未知结果，应按业务语义读取；底层持久化故障需停止服务、重开并确认恢复。

CLI 退出码：0 成功；1 已知业务错误或其他失败；2 参数错误；3 写入结果未知。
SIGINT/SIGTERM 在主线程同步等待，收到后先 drain RPC，再关闭 DB；五秒 grace period
用于 RPC 响应等待，miniRPC 最终仍会 join handler，不能保证所有底层阻塞 I/O 在五秒内结束。

## 验证与后续

`kv_rpc_integration_test` 使用真实 TCP 连接，覆盖 CRUD、二进制和空 value、
批次全量校验、上限边界、并发客户端、重开持久化、优雅停机，
以及“客户端超时但写入最终成功”和“WAL 错误返回 Unknown、重开后整批恢复”。
它使用 localhost:19879，CTest 以 `kv_rpc_listener` resource lock 串行化。

2026-11-04 本机验证结果（已有 `minirpc-linux`，Linux aarch64，GCC 13.3，
CMake 3.28.3，Protobuf 3.21.12；使用两个仓库当前工作区，包含既有未提交修改）：

以下为 2026-11-04 的历史结果，早于 SkipList 原子发布和去锁，未在本轮重新执行。
2026-11-07 仅验证纯 KV 的四种配置，见 [当前验收记录](KV_CLOSURE_PLAN.md#2026-11-07-原子发布与去锁验证)。

| 检查 | 结果 |
| --- | --- |
| miniRPC 原有 Release 构建与 CTest | 20/20 通过 |
| KV + RPC Debug 构建与完整 CTest | 7/7 通过 |
| `run_rpc_checks.sh` 从安装依赖到完整 CTest | 7/7 通过 |
| KV/适配层 ASan + UBSan，RPC 集成用例 | 1/1 通过，miniRPC 导入库未插桩 |
| 独立 CLI 进程 Put/Get/Write、错误退出码 | 通过 |
| CLI SIGTERM 停机后重启读取与删除持久化 | 通过 |
| RPC OFF 且禁用 Protobuf/miniRPC 查找的配置 | 通过，纯 KV 不触发这些依赖查找 |

这些结果不代表多机测试、性能基准或远端 CI 已执行。当前 CI 继续验证纯 KV；
RPC 组合验证使用上述脚本和相邻 miniRPC 源码。

当前不提供复制、Raft、分片、远程 Snapshot/Iterator、TLS 或鉴权。默认监听 loopback，
外网部署需要另行设计访问控制。后续 Raft 可放在 KVService 和 DB 之间，保持
“提交日志后再应用”的顺序，并为节点间 RPC 使用独立调度资源。

## HNSW 退役

向量源码、公共头文件、测试、探针、vector benchmark 与 week6 脚本已删除；
构建、CI 和活动 benchmark 入口改为纯 KV。现有目录中的业务字节数据与旧索引文件
不会自动删除，纯 KV 继续把 value 作为不透明字节。历史文档和 benchmark 记录仅作留档。
