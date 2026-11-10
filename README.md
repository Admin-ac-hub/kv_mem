# Mini LSM-KV

[![CI](https://github.com/Admin-ac-hub/kv_mem/actions/workflows/ci.yml/badge.svg)](https://github.com/Admin-ac-hub/kv_mem/actions/workflows/ci.yml)

Mini LSM-KV 是一个基于 C++20 实现的单机持久化 KV 存储引擎。项目采用 LSM-Tree 架构，覆盖从 WAL、MemTable、SSTable 到 Manifest、崩溃恢复和分层 Compaction 的完整数据生命周期，并实现 MVCC Snapshot、范围扫描、缓存与数据完整性校验。

除核心引擎外，仓库还提供自动化测试、随机压力测试、Sanitizer CI 和 benchmark，用于验证并发正确性、持久化语义与读写性能。

## 核心特性

| 方向 | 实现 |
| --- | --- |
| 写入路径 | 原子 `WriteBatch`、WAL append + fsync、SkipList MemTable、immutable MemTable、后台 Flush |
| 持久化 | 分块 SSTable、追加式 Manifest VersionEdit、`CURRENT` 指针、WAL/Manifest replay |
| 一致性 | 全局 sequence number、MVCC Snapshot、版本化 key、tombstone 可见性控制 |
| 读取路径 | Bloom Filter、内存 Block Index、restart-point seek、LRU Block Cache |
| 范围扫描 | MemTable/SSTable 子迭代器与 heap-based MergingIterator 在线归并 |
| Compaction | 简化 L0/L1/L2 leveled compaction，处理重叠范围、历史版本和 tombstone |
| 数据校验 | WAL record、Manifest record 和 SSTable DataBlock CRC32 校验 |
| 工程验证 | 单元测试、并发随机压力测试、ASan/UBSan/TSan CI、可复现 benchmark |

### miniRPC 服务

纯 KV 引擎可选链接相邻项目 miniRPC，提供单节点远程 `Get/Put/Delete/Write`。
RPC 采用独立适配层，支持二进制 value、原子批次、明确的业务错误与写入结果状态。
另有独立的固定三节点模式 `kv_cluster_server` / `kv_cluster_client`，提供 Raft 多数派复制、自动选主、故障切换和重启追赶。
三节点使用独立数据目录，不能用三个单节点 `kv_server` 代替；启动方法和安全/容量边界见 [三节点集群](docs/THREE_NODE_CLUSTER.md)。
2026-11-04 起移除 HNSW/VectorDB 的源码、构建目标及测试；原始设计和实验报告保留为历史记录。

## 系统架构

```text
Write path

  Put / Delete / WriteBatch
             |
             v
     WAL batch append + fsync
             |
             v
    active SkipList MemTable
             |
        size threshold
             |
             v
       immutable MemTable
             |
       background flush
             |
             v
          L0 SSTable --------+
             |               |
             +--> Compaction +--> L1 / L2 SSTable


Read path

  Get / Snapshot / Iterator
             |
             v
  active + immutable MemTable
             |
             v
        SSTable version
             |
      Bloom Filter check
             |
       Block Index lookup
             |
        LRU Block Cache
             |
     DataBlock decode + CRC32


Recovery path

  CURRENT -> MANIFEST replay -> open live SSTables
                                  |
                                  v
                         replay live WAL files
                                  |
                                  v
                          rebuild MemTable state
```

## 核心设计

### 写入与原子提交

`Put` 和 `Delete` 统一转换为 `WriteBatch`。并发请求进入 FIFO writer queue，由组首线程合并已经排队的完整批次，共用一次带 CRC 的 WAL batch record 和 fsync。不主动等待攒批；没有并发请求时仍立即提交原批次。WAL append 和 fsync 成功后再更新 MemTable；每个操作获得连续的 sequence number，读视图在整组更新完成后取得，恢复也以完整记录为单位解码和重放。

`Options::enable_write_grouping` 默认 `true`，设为 `false` 后每次只提交一个原始请求；两种模式都保持同步持久化。`write_group_max_bytes` 默认 1 MiB，必须大于零，限制合并后的操作编码 payload（不含 WAL header）；大于上限的合法原始批次单独提交，不会拆分。非法请求在入队前独立拒绝。`Close` 停止接收新写入，等待已接收请求完成后再关闭文件和后台线程。

`DBStats` 中 `wal_sync_count`、`wal_committed_operations` 和 `max_wal_batch_operations` 统计当前对象一生中成功同步到 WAL 的次数、操作数和最大批次操作数，跨 WAL 切换及 Close/Open 累计；空批次和恢复重放不计数。同步后的维护失败不会撤销这些 WAL 统计。

设计参考 [LevelDB 同步批量写入](https://github.com/google/leveldb/blob/main/doc/index.md)。并发与故障回归、基准方法和实测结果见 [自动批量提交报告](docs/GROUP_COMMIT_REPORT.md)。

2026-10-14 的七轮实测中，4/8 线程 KV 前台吞吐为旧实现的 2.08/3.15 倍，包含 Close 后为
1.48/1.56 倍，真实请求 P99 增加。这些测量早于 2026-11-07 的 SkipList 原子发布及去锁改动，
不能用来量化本次去锁的收益；报告中的向量实验属于已退役扩展的历史数据。

WAL 读写统一使用 batch 格式，不支持没有 batch envelope 的旧单操作记录。读取到完整但不匹配的 magic 时返回 `Corruption`。

错误返回不能推断为未提交：WAL 同步错误可能留下完整记录，MemTable 更新后的切换也可能失败。WAL I/O 或 Manifest 发布错误会阻止后续写入；应关闭、重开并通过读取确认结果，避免盲目重试。不提供内存分配失败下的强异常保证。

active MemTable 达到容量阈值后会切换为 immutable MemTable。前台写入立即进入新的 active MemTable，后台线程负责将 immutable MemTable 刷写为 L0 SSTable。新版本在 Manifest 持久化成功后才对恢复流程生效，旧 WAL 也只会在对应数据安全进入 SSTable 后清理。

### SSTable 与 Block 格式

SSTable 由 DataBlock、IndexBlock、FilterBlock 和 Footer 组成：

- DataBlock 保存按 `user key + sequence` 排序的版本化记录。
- 相邻 key 使用 restart interval 前缀压缩，降低重复前缀带来的空间开销。
- Block Index 常驻内存，通过 `lower_bound` 定位候选 DataBlock。
- Bloom Filter 用于快速排除确定不存在的 key，减少无效 block 读取。
- DataBlock trailer 保存编码类型和 CRC32，读取时校验数据完整性。
- LRU Block Cache 缓存热点 DataBlock，并记录 hit、miss 和实际 block read 指标。

点查在 block 内先对 restart points 二分搜索，再从最近的 restart point 顺序解码，避免扫描整个 DataBlock。

### MVCC 与范围扫描

每次写入分配单调递增的 sequence number。Snapshot 捕获创建时的最新 sequence，读取时只选择 `sequence <= read_sequence` 的最新版本，从而获得稳定的时间点视图。

`NewIterator()` 不会预先物化整个数据库：SSTable 按需读取数据块，MemTable 版本记录先复制到 vector。子迭代器通过最小堆在线归并，同一个 user key 的旧版本和不可见 tombstone 被跳过。接口支持 `SeekToFirst`、`Seek`、`Next`、`Valid`、`key`、`value` 和 `status`；扫描结束必须检查 `status()`。

### Manifest 与崩溃恢复

Manifest 使用追加式 VersionEdit 记录 file number、当前 WAL、SSTable level、key range 和文件大小，`CURRENT` 文件指向当前 MANIFEST。启动流程按以下顺序恢复状态：

1. 读取 `CURRENT` 并定位 MANIFEST。
2. replay VersionEdit，重建存储版本和 live file 集合。
3. 打开 Manifest 引用的 SSTable，忽略未发布的 orphan SSTable。
4. replay 仍存活的 WAL batch，恢复尚未 Flush 的记录。
5. 恢复 sequence 与 file number 分配器，继续接受写入。

WAL、Manifest 或 SSTable 校验失败时返回明确的 `Corruption`，避免静默使用损坏数据。

### Compaction

当前实现简化的 L0/L1/L2 leveled compaction：L0 文件允许 key range 重叠；向 L1/L2 输出时合并目标层的重叠文件，并维护非重叠 range。输入端使用 SSTable iterator 和最小堆做流式归并，避免一次性加载全部 SSTable 内容。

Compaction 会清理被覆盖的历史版本，但必须保留活跃 Snapshot 仍然可见的记录。tombstone 只有在确认更低层不存在需要屏蔽的旧值时才能删除。

### 并发模型

2026-11-07 起已移除 `write_mu_` 和 `memtable_mu_`。每个 DB 的线程锁共 6 把：
DB 直接声明 4 把，内部 WriteQueue 与 BlockCache 各 1 把。目录 `flock` 另计。

| 锁 | 保护范围 |
| --- | --- |
| WriteQueue `mu_` | 写入准入、选出唯一提交组首、完成通知 |
| `version_mu_` | Manifest、SSTable 版本与 Snapshot 列表 |
| `bg_mu_` | 后台 Flush/Compaction 的唤醒和停止状态 |
| `compaction_mu_` | 串行化公开与后台 Compaction |
| `lifecycle_mu_` | 串行化 Open/Close，包括后台线程 join 阶段 |
| BlockCache `mutex_` | 缓存内容、LRU 顺序与缓存统计 |

WriteQueue 保证同一时刻只有一个组首执行提交，关闭 grouping 后也保持串行提交。组首在释放队列锁后完成 WAL append、sequence 分配与 MemTable 更新，因此不再需要额外的 `write_mu_`。`Close()` 先停止写入准入并排空队列，再关闭文件和切换内存状态。

一次操作涉及的不同显式锁对象如下；同一把锁重复获取只算一把，各阶段并非同时持锁：

| 路径 | 锁数量 | 涉及的锁 |
| --- | ---: | --- |
| 普通 Get，内存表命中 | 0 | 原子固定 Version、读取序号和遍历节点 |
| 普通 Get，访问 SSTable 数据块 | 1 | BlockCache `mutex_`；范围或 Bloom 排除时可不访问缓存 |
| 带 Snapshot 的 Get | 1–2 | 增加 `version_mu_` 校验句柄 |
| 常规写入的提交组首 | 2 | WriteQueue `mu_`、`version_mu_` |
| 写入触发 MemTable 切换 | 3 | 再加 `bg_mu_` 通知后台 |

合并组内的跟随请求由组首代为提交，跟随线程只参与写队列同步。

写入先完成 WAL 同步和整批 MemTable 插入，再用一次 Release store 发布原子的 `last_sequence_`。普通 Get/NewIterator 以 Acquire 读取序号，并原子 pin 一个不可变 `Version`；Version 一起保存 active/immutable MemTable、SSTable 和对应范围元数据，取得视图不再逐表复制引用。若取序号期间 Version 已切换，则重新取视图，避免序号与数据来源不一致。

SkipList 支持一个外部串行化的写者与多个并发读者：节点的 key/value 和全部后继指针初始化后，按从底层到高层的顺序用 Release store 发布，读者用 Acquire load 遍历。高度与节点计数也使用原子变量。相同 internal key 的覆盖通过新节点替换，旧节点保留到整张表销毁；读请求固定的 Version 保证 MemTable 生命周期。因此无需 `memtable_mu_`，普通 Get/NewIterator 也不获取 `version_mu_`，WAL/MANIFEST 同步和节点插入不会通过这两把锁阻塞普通读取。Snapshot 句柄管理和校验仍使用版本锁，数据块访问仍使用缓存锁；原子 shared_ptr 操作不承诺在所有平台上无锁。

单写者由队列协议保证，发布指针使用 store，无需 CAS；多个写者直接并发修改同一 SkipList 不在契约内。
节点发布只保证读者看到完整节点，整组可见性仍由批末 `last_sequence_` 的 Release/Acquire 同步保证。

Compaction 先持久化 MANIFEST，再发布新 Version；旧读持有 Version 和 SSTable fd，旧路径删除后仍可完成。`Close()` 会停止后台线程并 drain 未完成的 immutable MemTable。

去锁回归覆盖半批安装时的旧视图、相同 internal key 的并发替换、多写线程与 Snapshot/Iterator、
切表和关闭排空。2026-11-07 本机 Debug、Release、ASan/UBSan、TSan 均 8/8 通过，
证据见 [当前验收记录](docs/KV_CLOSURE_PLAN.md#2026-11-07-原子发布与去锁验证)。本次未做吞吐对比。

## 文件布局

一个数据库目录包含以下文件：

```text
CURRENT
MANIFEST
LOCK
wal_000003.log
sst_000004.data
sst_000005.data
```

源码目录：

```text
include/    public headers
src/        storage engine implementation
rpc/        optional miniRPC adapter, protocol, client/server and integration test
test/       deterministic unit and concurrency tests
stress/     randomized concurrent model-based stress test
bench/      benchmark workloads
cmake/      CMake helper modules
scripts/    benchmark and stress-test entrypoints
docs/       design notes and benchmark analysis
```

## 构建与测试

依赖 CMake 3.16+ 和支持 C++20 的编译器。

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

启用 AddressSanitizer 和 UndefinedBehaviorSanitizer：

```bash
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DKV_SANITIZERS=address,undefined
cmake --build build-sanitized --parallel
ctest --test-dir build-sanitized --output-on-failure
```

GitHub Actions 在 Ubuntu/macOS 运行纯 KV Debug/Release、ASan/UBSan，并在
Ubuntu 运行 TSan。核心目标为 `kv_core`，`kvdb` 是它的 CMake alias。
RPC 使用 Linux epoll/eventfd，默认 `KV_ENABLE_RPC=OFF`；开启后额外依赖 miniRPC 与 Protobuf。

## miniRPC 远程服务

在 Linux 中先安装 miniRPC，再构建当前项目（以下从 KV 仓库根目录执行）：

```bash
cmake -S ../miniRPC -B ../miniRPC/build -DCMAKE_BUILD_TYPE=Release
cmake --build ../miniRPC/build --parallel
cmake --install ../miniRPC/build --prefix /tmp/minirpc-prefix

cmake -S . -B build-rpc -DCMAKE_BUILD_TYPE=Debug \
  -DKV_ENABLE_RPC=ON -DCMAKE_PREFIX_PATH=/tmp/minirpc-prefix
cmake --build build-rpc --parallel
ctest --test-dir build-rpc --output-on-failure
```

启动服务及调用（客户端在另一个终端执行）：

```bash
./build-rpc/rpc/kv_server ./rpc_db 127.0.0.1 9000
./build-rpc/rpc/kv_client 127.0.0.1 9000 put user:1 alice
./build-rpc/rpc/kv_client 127.0.0.1 9000 get user:1
./build-rpc/rpc/kv_client 127.0.0.1 9000 write put user:2 bob delete user:1
```

服务接收 SIGINT/SIGTERM 后先停止接收请求并等待 handler 收尾，再关闭 DB。
客户端 CLI 的 `write` 将所有操作作为一次原子批次提交。C++ API 支持任意二进制 value；
命令行参数不能表达 NUL 字节。

本机 macOS 可使用已有 `minirpc-linux` 容器运行 `./scripts/run_rpc_checks.sh`；
脚本不创建容器，保留 Linux 构建位置并输出后续启动命令。
协议、限制、故障语义与验证方法见 [miniRPC 接入说明](docs/MINIRPC_INTEGRATION.md)。

## 随机压力测试

压力测试使用多线程随机生成 `Put`、`Delete` 和 `WriteBatch`，每个 epoch 都会通过 `NewIterator()` 与内存模型进行全量对拍，并周期性执行 reopen 和 Compaction，覆盖并发写入、后台 Flush、tombstone、恢复与迭代器归并路径。

```bash
./scripts/stress_test.sh
```

可通过环境变量调整 workload：

```bash
EPOCHS=20 \
THREADS=8 \
OPS_PER_THREAD=5000 \
REOPEN_EVERY_EPOCHS=2 \
COMPACT_EVERY_EPOCHS=3 \
./scripts/stress_test.sh
```

Docker 运行：

```bash
docker build -t mini-lsm-kv-stress .
docker run --rm mini-lsm-kv-stress
```

## Benchmark

Benchmark 默认使用 1024 条 MemTable、64 MiB L1 字节预算和 block cache，并打印
实际 flush/compaction 次数；写入吞吐包含 `Close()` 时的后台收尾。

Benchmark 覆盖写入、顺序读、随机读、范围扫描和混合读写 workload，输出以下指标：

- QPS 与平均延迟
- 范围扫描吞吐
- SSTable 数量与 Compaction 次数
- Block Cache hit/miss
- Bloom Filter 过滤次数
- DataBlock 实际读取次数
- 数据库目录占用空间

```bash
./scripts/run_benchmarks.sh
```

workload 定义、指标解释和结果分析见 [docs/BENCHMARK_ANALYSIS.md](docs/BENCHMARK_ANALYSIS.md)。性能结果与硬件、文件系统、编译器和 OS page cache 强相关，比较时应固定 commit 和测试环境并进行多轮采样。

第五周的同步、批处理和 Sanitizer 实验见
[docs/WEEK5_PERFORMANCE_REPORT.md](docs/WEEK5_PERFORMANCE_REPORT.md)，可用
`./scripts/run_week5_experiment.sh` 重现 CSV 数据。

## API 概览

```cpp
#include <string>

#include "db.h"
#include "write_batch.h"

int main() {
  kv::Options db_options;
  db_options.db_path = "./example_db";
  kv::DB db(db_options);
  if (!db.Open().ok()) {
    return 1;
  }

  if (!db.Put("user:1", "alice").ok()) {
    return 1;
  }

  kv::WriteBatch batch;
  batch.Put("user:2", "bob");
  batch.Delete("user:1");
  if (!db.Write(batch).ok()) {
    return 1;
  }

  const kv::Snapshot* snapshot = db.GetSnapshot();
  kv::ReadOptions options;
  options.snapshot = snapshot;

  std::string value;
  if (!db.Get("user:2", &value, options).ok()) {
    return 1;
  }
  db.ReleaseSnapshot(snapshot);

  return db.Close().ok() ? 0 : 1;
}
```

主要接口包括 `Open`、`Close`、`Write`、`Put`、`Get`、`Delete`、`Compact`、
`Stats`、`GetSnapshot`、`ReleaseSnapshot` 和 `NewIterator`。

`Open()` 校验主要容量参数：MemTable/L0/L1 阈值和 block_size 必须为正，block_size
不超过 uint32 范围，Bloom bits/key 为 1–64；block cache 容量按块数计，0 可禁用。
路径不能为空，普通文件冲突等 filesystem 错误转换为 `Status`。
`create_if_missing/error_if_exists` 当前按目录是否存在判断，空的已有目录可以初始化。
key 不能为空且不能包含 tab、newline 或 NUL；value 允许任意字节。

数据库目录使用保留在磁盘的 `LOCK` 文件与 POSIX `flock`，拒绝同目录第二个协作实例。
关闭或失败 Open 会释放锁；不要在实例存活期间外部删除 LOCK 或修改目录内容。
重复 Open 返回 InvalidArgument，重复 Close 允许，同一个对象可重新 Open。
未打开时 Get/Write/Compact 拒绝，空 WriteBatch 也检查状态；GetSnapshot 返回 nullptr。
已创建的 Iterator 可在 Close 后读取其固定视图，新 Iterator 返回 IOError；
Iterator 和 Snapshot 都应在 DB 销毁前释放。

当前已验证边界、未完成项目和面试材料入口见 [KV 主线收尾计划](docs/KV_CLOSURE_PLAN.md)。

## 当前边界

- 单实例读写引擎，同一路径的第二个协作实例通过目录锁拒绝；不支持多个实例共享写入。
- Compaction 尚未实现完整的 level score、grandparent overlap 控制和多文件输出切分。
- Compaction 输入端已流式归并，输出端仍会先构建内存 vector 再生成 SSTable。
- 写入始终同步持久化；低并发下自动合并机会有限，尚无异步 durability 策略。
- DataBlock 已保留编码类型，但当前只写入原始 payload，尚未接入 Snappy 或 Zstd。
- immutable 队列尚无完整背压；Compaction 输出全量物化，慢维护和长 Snapshot 仍可能增加内存与空间占用。
- 已覆盖恢复与故障注入场景，完整发布点断电矩阵、解码器模糊测试和长期稳态验证仍未完成。
- 原有 `kv_server` 仍是单节点；新增三节点模式使用 Raft，但固定成员、完整历史重写且上限 64 MiB，尚无快照压缩、分片、请求去重、TLS 或鉴权。详见 [三节点边界](docs/THREE_NODE_CLUSTER.md#第一版容量与部署边界)。
- RPC 单 value 上限 1 MiB、序列化请求 payload 上限 1.5 MiB、单批次最多 4096 操作；本地 DB 不受这些 RPC 限制。
- 写入超时或底层 I/O 错误可能对应已提交数据；客户端不自动重放写入。

## 后续方向

- 实现按 level score 和文件 overlap 选择输入的 Compaction Picker。
- 增加流式 SSTable Builder 和目标文件大小控制。
- 增加可配置同步策略，并评估持续写入时的维护背压。
- 接入 Snappy/Zstd，对比压缩率、CPU 开销与缓存命中率。
- 增加进程异常退出、I/O 故障注入和长时间稳定性测试。
