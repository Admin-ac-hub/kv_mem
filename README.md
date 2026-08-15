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

## API 概览

```cpp
#include <string>

#include "db.h"
#include "write_batch.h"

int main() {
  kv::DB db("./example_db");
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

主要接口包括 `Open`、`Close`、`Write`、`Put`、`Get`、`Delete`、`Compact`、`Stats`、`GetSnapshot` 和 `NewIterator`。

## 当前边界

- 单进程存储引擎，同一路径不支持多个 `DB` 实例并发写入。
- Compaction 尚未实现完整的 level score、grandparent overlap 控制和多文件输出切分。
- Compaction 输入端已流式归并，输出端仍会先构建内存 vector 再生成 SSTable。
- 每个 WriteBatch 默认执行一次 WAL fsync，尚未实现 writer queue、group commit 和可配置 durability。
- DataBlock 已保留编码类型，但当前只写入原始 payload，尚未接入 Snappy 或 Zstd。

## 后续方向

- 实现按 level score 和文件 overlap 选择输入的 Compaction Picker。
- 增加流式 SSTable Builder 和目标文件大小控制。
- 实现 writer queue、group commit 与可配置同步策略。
- 接入 Snappy/Zstd，对比压缩率、CPU 开销与缓存命中率。
- 增加进程异常退出、I/O 故障注入和长时间稳定性测试。
