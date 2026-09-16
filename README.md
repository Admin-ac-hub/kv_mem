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
| 向量存储 | Float32 固定维度向量、JSON metadata、L2/IP/Cosine 精确 Top-K 基线 |
| ANN 索引 | HNSW、连续向量布局、AVX2 / AArch64 NEON 距离加速、单次读锁批量查询、可配置 `M`/`ef` |
| 工程验证 | 单元测试、并发随机压力测试、ASan/UBSan/TSan CI、可复现 benchmark |

### 向量存储（第一阶段）

向量记录与普通 KV 共用 WAL、MemTable、SSTable、MVCC Snapshot 和
Compaction 路径。`Options::vector_dimension` 可约束数据库中的固定向量维度；
设为 `0` 时不在写入阶段强制维度，但每次搜索仍只比较与查询同维的向量。
metadata 作为不透明字符串持久化，调用方负责保证传入内容是合法 JSON。

当前 `BruteForceSearch` 会扫描快照中的最新可见记录，并使用有界 Top-K 堆返回
精确结果，可作为后续 HNSW 的 Recall 基线。距离统一采用值越小越相近的语义：
L2 返回欧氏距离，Inner Product 返回负内积，Cosine 返回 `1 - cosine_similarity`。

### HNSW 索引（第二阶段）

`HNSWIndex` 实现了独立的内存 ANN 索引，包括指数分布随机层级、上层贪心下降、
层内 beam search、启发式邻居选择和双向连边。`HNSWOptions` 可配置固定维度、
`max_neighbors`、`ef_construction`、距离类型和随机种子；查询通过
`ef_search` 控制召回率与延迟的权衡。插入使用独占锁，查询使用共享锁。

固定种子的 `1000 x 128` 测试会以精确搜索为基线验证 `Recall@10 > 0.90`，
并检查平均查询延迟低于 1 ms。`DB::Search` 已接入 HNSW，并在查询阶段按
LSM 的最新版本、tombstone 和 snapshot 过滤结果。Flush/Compaction 会生成带 CRC32
校验的 HNSW 子索引，Manifest 记录索引路径，重启时按 SSTable 顺序加载并合并。

Compaction 采用两阶段发布：先固定输入 SSTable，在不持有全局版本锁的情况下
归并数据并构建替换索引，此时查询继续使用旧索引；发布前短暂持锁，补入构建期间
新增的 SSTable 和 MemTable 版本，持久化 Manifest 后原子替换内存图。旧 SSTable
与索引文件只在新版本发布成功后删除。

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

`Put` 和 `Delete` 统一转换为 `WriteBatch`。每个 batch 在提交时获得连续的 sequence number，并编码为一条 WAL batch record。只有 WAL append 和 fsync 成功后，batch 才会写入 MemTable，因此恢复过程不会看到半个 batch。

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

`NewIterator()` 不会预先物化整个数据库。它为 active MemTable、immutable MemTable 和各 SSTable 创建子迭代器，再通过最小堆在线归并；同一个 user key 的旧版本和不可见 tombstone 会在归并过程中被跳过。接口支持 `SeekToFirst`、`Seek`、`Next`、`Valid`、`key`、`value` 和 `status`。

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

读取在拿到 MemTable 和 SSTable 版本快照后释放全局版本锁，SSTable I/O 不长期占用 DB 状态锁。读请求通过 `shared_ptr` pin 当前 SSTable 版本，因此 Compaction 发布新版本并删除旧文件时，已经开始的读取仍可安全完成。`Close()` 会停止后台线程并 drain 未完成的 immutable MemTable，确保资源释放与目录清理之间没有竞态。

## 文件布局

一个数据库目录包含以下文件：

```text
CURRENT
MANIFEST
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

GitHub Actions 会在 Ubuntu 和 macOS 上运行 Debug/Release 构建，并额外执行 ASan、UBSan 和 TSan 检查。

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

向量正式验收尚未完成，路线图目标不是实测指标。向量 benchmark 默认保留
正常的 1024 条 MemTable、64 MiB L1 字节预算和 block cache，并打印实际
flush/compaction 次数；写入吞吐包含 Close 时的后台收尾。复现方式与批量延迟
口径见 [向量 Benchmark 报告](docs/WEEK6_BENCHMARK_REPORT.md)。

HNSW 的 `M` 限制上层邻居数，level 0 上限为 `2*M`。索引保存使用 v2 格式，
可读取旧 v1 文件；旧程序不能读取新的 v2 文件。`DB::VectorIndexStats()` 提供
图统计（O(节点数 + 边数)）；`Options::level1_size_limit_bytes` 独立配置 L1 预算。
大规模召回回归可通过 `-DKV_ENABLE_RECALL_REGRESSION=ON` 启用，再运行
`ctest --test-dir <build目录> -L recall --output-on-failure`。这仍不能替代百万规模验收。

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

向量工作负载的 Week 6 benchmark 覆盖百万级写入、千次随机查询和 50/50
混合读写，默认维度为 768。它会输出吞吐、P50/P99、精确 Recall@10 抽样和
SSTable/HNSW 文件大小：

```bash
./scripts/run_week6_benchmark.sh
```

完整方法、开发机基线和正式 1M x 768 复现实验见
[docs/WEEK6_BENCHMARK_REPORT.md](docs/WEEK6_BENCHMARK_REPORT.md)。

## API 概览

```cpp
#include <string>
#include <vector>

#include "db.h"
#include "vector_value.h"
#include "write_batch.h"

int main() {
  kv::Options db_options;
  db_options.db_path = "./example_db";
  db_options.vector_dimension = 3;
  kv::DB db(db_options);
  if (!db.Open().ok()) {
    return 1;
  }

  if (!db.Put("user:1", "alice").ok()) {
    return 1;
  }

  if (!db.PutVector("doc:1", {1.0f, 0.0f, 0.0f},
                    R"({"category":"tech"})").ok()) {
    return 1;
  }

  std::vector<kv::VectorResult> nearest;
  if (!db.BruteForceSearch({0.9f, 0.1f, 0.0f}, 10, &nearest,
                           kv::VectorDistanceMetric::kCosine).ok()) {
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

主要接口包括 `Open`、`Close`、`Write`、`Put`、`Get`、`PutVector`、
`GetVector`、`BruteForceSearch`、`Search`、`SearchBatch`、`Delete`、`Compact`、`Stats`、
`VectorIndexStats`、`GetSnapshot` 和 `NewIterator`。

## 当前边界

- 单进程存储引擎，同一路径不支持多个 `DB` 实例并发写入。
- Compaction 尚未实现完整的 level score、grandparent overlap 控制和多文件输出切分。
- Compaction 输入端已流式归并，输出端仍会先构建内存 vector 再生成 SSTable。
- HNSW Compaction 当前构建完整替换索引，尚未实现只更新变化节点的增量算法。
- HNSW 插入由独占锁串行执行；Flush 子索引和 Compaction 重建仍有重复构图成本。
- metadata 预过滤、范围与向量组合、分数融合及完整的引擎查询/插入指标尚未实现。
- 每个 WriteBatch 默认执行一次 WAL fsync，尚未实现 writer queue、group commit 和可配置 durability。
- DataBlock 已保留编码类型，但当前只写入原始 payload，尚未接入 Snappy 或 Zstd。
- HNSW 子索引会随 SSTable Flush/Compaction 持久化；重启时优先加载并合并已发布的索引文件，老版本数据库仍可通过 SSTable 全量重建。

## 后续方向

- 实现按 level score 和文件 overlap 选择输入的 Compaction Picker。
- 增加流式 SSTable Builder 和目标文件大小控制。
- 实现 writer queue、group commit 与可配置同步策略。
- 接入 Snappy/Zstd，对比压缩率、CPU 开销与缓存命中率。
- 增加进程异常退出、I/O 故障注入和长时间稳定性测试。
