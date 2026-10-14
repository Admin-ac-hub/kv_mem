# Mini LSM-KV

[![CI](https://github.com/Admin-ac-hub/kv_mem/actions/workflows/ci.yml/badge.svg)](https://github.com/Admin-ac-hub/kv_mem/actions/workflows/ci.yml)

Mini LSM-KV 是一个基于 C++17 实现的单机持久化 KV 存储引擎。项目采用 LSM-Tree 架构，覆盖从 WAL、MemTable、SSTable 到 Manifest、崩溃恢复和分层 Compaction 的完整数据生命周期，并实现 MVCC Snapshot、范围扫描、缓存与数据完整性校验。

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

### 可选向量检索扩展

默认构建只提供纯 KV。显式启用 `KV_ENABLE_VECTOR_EXTENSION=ON` 后，
`VectorDB` 组合 KV 与独立 HNSW 索引，提供向量读写、精确 Top-K、近似搜索和
历史 Snapshot 搜索。KV 核心不解析向量，也不访问或维护索引文件。

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

`Put` 和 `Delete` 统一转换为 `WriteBatch`。每个非空 batch 在提交时获得连续的 sequence number，并编码为一条带 CRC 的 WAL batch record。WAL append 和 fsync 成功后再更新 MemTable；读视图在整批更新完成后取得，恢复也以完整记录为单位解码和重放。

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

引擎使用职责分离的锁控制共享状态：

| 锁 | 保护范围 |
| --- | --- |
| `write_mu_` | WAL append、sequence 分配和写入顺序 |
| `memtable_mu_` | active/immutable MemTable 的并发访问 |
| `version_mu_` | Manifest、SSTable 版本与 Snapshot 列表 |
| `bg_mu_` | 后台 Flush/Compaction 的唤醒和停止状态 |
| `compaction_mu_` | 串行化公开与后台 Compaction |
| `lifecycle_mu_` | 串行化 Open/Close，包括后台线程 join 阶段 |

读取在拿到 MemTable 和 SSTable 版本快照后释放全局版本锁，SSTable I/O 不长期占用 DB 状态锁。读请求通过 `shared_ptr` pin 当前 SSTable 版本，因此 Compaction 发布新版本并删除旧文件时，已经开始的读取仍可安全完成。`Close()` 会停止后台线程并 drain 未完成的 immutable MemTable，确保资源释放与目录清理之间没有竞态。

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
test/       deterministic unit and concurrency tests
stress/     randomized concurrent model-based stress test
bench/      benchmark workloads
cmake/      CMake helper modules
scripts/    benchmark and stress-test entrypoints
docs/       design notes and benchmark analysis
```

## 构建与测试

依赖 CMake 3.16+ 和支持 C++17 的编译器。

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

GitHub Actions 分别构建纯 KV 和向量扩展，在 Ubuntu/macOS 运行 Debug/Release
及 ASan/UBSan，并在 Ubuntu 运行 TSan。核心目标为 `kv_core`，`kvdb` 是它的
CMake alias；直接链接静态库的调用方应迁移到 `libkv_core` 和 `libkv_common`。

启用向量扩展：

```bash
cmake -S . -B build-vector -DCMAKE_BUILD_TYPE=Release \
  -DKV_ENABLE_VECTOR_EXTENSION=ON
cmake --build build-vector --parallel
ctest --test-dir build-vector --output-on-failure
```

算法测试/探针只链接 `hnsw`（依赖 `vector_value` 和基础工具 `kv_common`），
不链接 KV。`KV_ENABLE_SIMD` 和 `KV_HNSW_DISTANCE_COUNTER` 只影响扩展目标。
`KV_ENABLE_RECALL_REGRESSION=ON` 要求同时启用扩展。

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

## 向量 API 迁移与一致性

旧 `DB::PutVector/GetVector/BruteForceSearch/Search/SearchBatch/VectorIndexStats`
移到 `VectorDB`；`WriteBatch::PutVector` 改为 `VectorWriteBatch::PutVector`。
KV 配置放在 `VectorOptions::kv_options`，向量维度、度量和 HNSW 参数直接放在
`VectorOptions`。`Options` 不再包含向量字段，聚合初始化调用方需要检查参数位置。

```cpp
#include "vector_db.h"

int main() {
  kv::VectorOptions options;
  options.kv_options.db_path = "./vector_example";
  options.vector_dimension = 3;
  kv::VectorDB db(options);
  if (!db.Open().ok()) return 1;

  kv::VectorWriteBatch batch;
  batch.Put("description", "example");
  if (!batch.PutVector("v1", {1, 0, 0}, "metadata").ok()) return 1;
  const auto write = db.Write(batch);
  if (!write.ok()) {
    // Inspect commit_state, kv_status and index_status before deciding to retry.
    // Committed means all KV operations are stored, even if the index failed.
    return 2;
  }
  std::vector<kv::VectorResult> results;
  if (!db.Search({1, 0, 0}, 10, &results).ok()) return 3;
  return db.Close().ok() ? 0 : 4;
}
```

应用链接 `vector_db`。扩展不暴露可写底层 DB；同一实例的写入必须经过扩展。
批次提交前校验 key、编码、维度、有限值和度量；新向量按普通 Put 写入 WAL。
扩展用协调锁覆盖整批 KV 提交与索引更新，查询不会看到半批次。
带 `KVV1` 魔数的普通 Put 在扩展中也按向量校验并索引，保证重开前后解释一致；
纯 KV 接受任意字节，包括语义错误的向量值。

`Write` 和 `PutVector` 返回 `VectorWriteResult`：

| commit_state | 含义与处理 |
| --- | --- |
| `NotCommitted` | 提交前拒绝；检查 `kv_status`，该调用没有写 WAL |
| `Committed` | KV 成功；`index_status` 失败表示索引 Dirty，数据已提交 |
| `Unknown` | 底层 KV 错误使提交结果无法确定；不能据此假设数据未写入 |

Dirty 状态下 `Search/SearchBatch` 返回索引不可用错误并清空结果；`Get`、
`GetVector`、Iterator 和 `BruteForceSearch` 仍从 KV 读取。显式 `RebuildIndex()`
从一致 KV 视图构建新图，成功后恢复 Ready。KV 仍可写时，Dirty 期间可继续提交批次，
结果明确表达“KV 已提交，索引不可用”。底层 KV 持久化错误需要 Close/Open 确认恢复；
单独重建索引不会清除 KV 的写入错误。普通 `Put/Delete` 保留 `Status` 返回约定；
其提交后失败消息包含 `KV committed`，需要结构化提交结果时使用 `Write`。

当前视图使用维护中的图；历史 Snapshot 搜索从该 Snapshot 的 Iterator 构建
临时图，`SearchBatch` 复用一次构图。历史搜索成本随该视图的向量数增长。
`index_rebuild_operations` 默认每 4096 次操作重建以回收旧节点与 tombstone，
设为 0 可关闭自动回收；回收与 KV Compaction 独立。重建阻止扩展写入，后台
KV Flush/Compaction 可以继续。

Snapshot 必须来自同一 DB、仍未释放且属于当前 Open 周期。Close 后快照失效，
可重复 Close 和重新 Open。释放的 Snapshot 小句柄保留到 DB 销毁，以防原始
指针地址重用；它们不再保留 KV 历史版本。Iterator/快照应在其 DB 销毁前释放。

磁盘迁移保持 WAL envelope、SSTable 和向量 value 布局；旧批次类型 3 作为普通
Put 解码，旧 Manifest 索引字段校验后忽略。旧 `index_*.hnsw` 保留原地，不读取、
更新或自动删除；首版每次 Open 从 KV 重建，不生成新索引文件。
兼容样本、双模式测试与性能口径见
[分离验收记录](docs/KV_HNSW_SEPARATION_REPORT.md)。旧二进制回退请使用备份，
小规模反向读取实验不构成通用双向兼容承诺。

## 当前边界

- 单实例读写引擎，同一路径的第二个协作实例通过目录锁拒绝；不支持多个实例共享写入。
- Compaction 尚未实现完整的 level score、grandparent overlap 控制和多文件输出切分。
- Compaction 输入端已流式归并，输出端仍会先构建内存 vector 再生成 SSTable。
- 每个 WriteBatch 默认执行一次 WAL fsync，尚未实现 writer queue、group commit 和可配置 durability。
- DataBlock 已保留编码类型，但当前只写入原始 payload，尚未接入 Snappy 或 Zstd。
- immutable 队列尚无完整背压；Compaction 输出全量物化，慢维护和长 Snapshot 仍可能增加内存与空间占用。
- 已覆盖恢复与故障注入场景，完整发布点断电矩阵、解码器模糊测试和长期稳态验证仍未完成。
- 可选向量扩展的 HNSW 图不持久化，重启需要完整重建；历史 Snapshot 搜索需要临时构图。

## 后续方向

- 实现按 level score 和文件 overlap 选择输入的 Compaction Picker。
- 增加流式 SSTable Builder 和目标文件大小控制。
- 实现 writer queue、group commit 与可配置同步策略。
- 接入 Snappy/Zstd，对比压缩率、CPU 开销与缓存命中率。
- 增加进程异常退出、I/O 故障注入和长时间稳定性测试。
