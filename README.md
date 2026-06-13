# Mini LSM-KV

一个用 C++17 实现的单机 LSM-Tree KV 存储引擎。项目目标不是复刻完整 LevelDB，而是把存储引擎面试里最核心的链路做完整：写前日志、MemTable、SSTable、Manifest、崩溃恢复、MVCC Snapshot、Iterator、Bloom Filter、Block Cache、Compaction 和可复现 benchmark。

## 适合写在简历上的版本

> 基于 C++17 实现单机 LSM-Tree KV 存储引擎，支持 WriteBatch 原子提交、WAL batch 崩溃恢复、SkipList MemTable、immutable MemTable、后台 Flush、分块 SSTable、MVCC Snapshot、lazy MergingIterator 范围扫描、Bloom Filter、LRU Block Cache、追加式 Manifest VersionEdit、CURRENT/MANIFEST replay 与简化 L0/L1/L2 Compaction；为 WAL、SSTable DataBlock、Manifest 增加 CRC32 校验，并提供单元测试和 benchmark 统计 QPS、延迟、范围扫描吞吐、缓存命中、Bloom 过滤和磁盘占用。

## 项目亮点

- **完整写入链路**：`Put/Delete` 复用 `WriteBatch`，一次 WAL fsync 可提交多条 Put/Delete；active MemTable 达阈值后切成 immutable，前台写入进入新的 active MemTable。
- **后台 Flush**：后台线程把 immutable MemTable 写成 L0 SSTable，Manifest fsync 成功后再清理旧 WAL；`Close()` 会 drain active/immutable，保证重启不丢数据。
- **MVCC 一致性读**：每次写入分配递增 sequence number，WAL、MemTable、SSTable 和 Manifest 都持久化版本信息，Snapshot 按 sequence 读取稳定历史视图。
- **范围扫描 Iterator**：`NewIterator()` 使用 MemTable/SSTable 子 iterator 和 heap-based MergingIterator 在线归并，支持 `SeekToFirst / Seek / Next`，并按 sequence 跳过旧版本和 tombstone。
- **可恢复元数据**：Manifest 采用追加 VersionEdit log，CURRENT 指向当前 MANIFEST；启动时 replay edits 重建版本，并忽略不在 Manifest 中的 orphan SSTable。
- **崩溃安全基础设施**：WAL record、Manifest 和 SSTable DataBlock 都带 CRC32，损坏数据返回 `Corruption`，避免静默读错。
- **读路径优化**：读取时先查 MemTable，再按新到旧查询 SSTable；SSTable 使用 Bloom Filter 跳过确定不存在的 key，通过内存 index 定位 DataBlock，并用 LRU Block Cache 缓存热点块。
- **Compaction 语义**：实现简化 L0/L1/L2 leveled compaction；L0 可重叠，L1/L2 输出为不重叠 range，输入端使用 SSTable iterator 做流式 heap merge，合并时保留活跃 Snapshot 仍可见的历史版本，没有活跃 Snapshot 时清理旧版本和可丢弃 tombstone。
- **Block 编码**：DataBlock 使用 restart interval 前缀压缩，`Get` 在 block 内通过 restart points 二分定位，再线性扫描一个 restart 区间；block trailer 记录 compression type 与 checksum，当前内置 none/fake codec，不依赖外部压缩库。
- **事件日志**：`Options::enable_event_log` 开启后会输出 recovery、memtable switch、flush、compaction 和 Manifest append 等关键事件，默认关闭以保持测试和 benchmark 输出干净。
- **可观测 benchmark**：输出 QPS、平均延迟、范围扫描条数、SSTable 数量、Compaction 次数、缓存命中/未命中、Bloom Filter 过滤次数、DataBlock 读取次数和目录大小。

## 架构

```text
Write:
  Client
    -> DB::Put/Delete/Write(WriteBatch)
    -> WAL batch append + fsync
    -> active SkipList MemTable
    -> switch to immutable MemTable
    -> background Flush
    -> SSTable prefix-compressed data blocks + index + filter + footer
    -> Manifest VersionEdit append + fsync

Read:
  Client
    -> DB::Get
    -> MemTable
    -> SSTable Bloom Filter
    -> SSTable in-memory index
    -> LRU Block Cache
    -> DataBlock + CRC32 verify

Snapshot / Iterator:
  DB::GetSnapshot
    -> capture latest sequence number
    -> ReadOptions
    -> stable point-in-time Get / Iterator

Recovery:
  DB::Open
    -> Read CURRENT
    -> Replay Manifest VersionEdits
    -> Open SSTables
    -> Replay live WAL files
    -> Rebuild MemTable
```

## 核心能力

- API：`DB::Open / Close / Write / Put / Get / Delete / Compact / Stats / GetSnapshot / NewIterator`
- WriteBatch：支持一次提交多个 Put/Delete，batch 内恢复原子性由 WAL batch record 保证
- WAL：二进制 batch record，包含 magic/type、sequence、count、payload size、CRC32 和 payload
- MemTable：基于 SkipList，按 user key + sequence 保存多版本记录
- SSTable：DataBlock + IndexBlock + FilterBlock + Footer 格式
- Block Index：打开 SSTable 时加载到内存，读请求通过 `lower_bound` 定位 block
- Snapshot：捕获当前 sequence number，支持稳定历史读视图
- Iterator：多路在线归并 active/immutable/SSTable iterator，在读视图下输出 key 升序的最新可见非删除版本
- Bloom Filter：按 SSTable 构建，减少无效磁盘 block 读取
- Block Cache：进程内 LRU 缓存热点 DataBlock
- Manifest：追加 VersionEdit log，记录 WAL、file number、SSTable level/range/size，CURRENT 损坏或缺失会返回明确错误或触发目录恢复
- Compaction：简化 L0/L1/L2 leveled compaction，使用 SSTable iterator 流式归并输入，感知活跃 Snapshot，避免清理仍可见版本
- 测试：覆盖 WAL batch 恢复、截断 WAL、后台 Flush、版本覆盖、删除语义、Snapshot、lazy Iterator、Manifest replay/截断/orphan、CRC 损坏检测、Bloom/Cache 统计、Compaction、Prefix Compression 和并发 Put/Get

## 目录结构

```text
include/       public headers
src/           storage engine implementation
test/          unit tests
test/test_dbs/ temporary DBs created by tests (ignored)
bench/         benchmark binary
scripts/       helper scripts for reproducible runs
docs/          interview notes
```

数据库目录示例：

```text
MANIFEST
CURRENT
wal_000003.log
sst_000001.data
sst_000002.data
```

## 编译

```bash
cmake -S . -B build
cmake --build build
```

## 运行测试

```bash
./build/kv_test
```

期望输出：

```text
kv_test passed
```

## 运行 benchmark

单独运行：

```bash
./build/kv_bench --path ./bench_db --write 100000 --value-size 100
./build/kv_bench --path ./bench_db --read 100000
./build/kv_bench --path ./bench_db --read-random 100000 --seed 1
./build/kv_bench --path ./bench_db --scan 100000
./build/kv_bench --path ./bench_db --mixed 100000 --value-size 100
```

一键运行推荐场景：

```bash
./scripts/run_benchmarks.sh
```

benchmark 输出字段：

```text
writes: 100000
reads: 0
scan_items: 0
elapsed_sec: ...
qps: ...
avg_latency_us: ...
sstables: ...
compactions: ...
cache_hits: ...
cache_misses: ...
bloom_filtered: ...
block_reads: ...
db_size_bytes: ...
```

内部测试统计还覆盖 `sstable_full_scans` 与 `block_restart_seeks`，用于防止 compaction 回退到全表 materialize，以及防止 prefix-compressed block 读取绕过 restart point seek。

## 面试讲解建议

详细讲法见 [docs/INTERVIEW.md](docs/INTERVIEW.md)。建议重点讲 4 条线：

1. 为什么选择 LSM：顺序写 WAL + 批量 flush，牺牲读放大换写入吞吐。
2. 写入如何保证可恢复：WAL 先于 MemTable，Manifest 记录版本，启动重放 WAL。
3. MVCC 如何支持 Snapshot：sequence number、读视图、tombstone 可见性。
4. 读路径如何优化：MemTable、Bloom Filter、IndexBlock、Block Cache 的层层过滤。
5. Compaction 解决什么问题：合并多份 SSTable，消除旧版本和 tombstone，同时保护活跃 Snapshot。

## 当前限制

- 只实现单进程写入，不支持多个 DB 实例同时写同一路径。
- Compaction 是简化版 L0/L1/L2 策略，尚未实现 LevelDB 的精细 picking、grandparent overlap 控制和多文件输出切分。
- `DB` 已拆出 `write_mu_`、`memtable_mu_`、`version_mu_` 和 `bg_mu_`：写入按 WAL/sequence 串行，Get 只短暂持版本锁拿快照，读 MemTable 用 shared lock，SSTable I/O 不持 DB 全局状态锁，后台线程唤醒/停止信号由独立 bg 锁管理。
- CRC32 用于损坏检测，不用于安全防篡改。
- Block Compression 目前提供 none/fake codec 抽象，未引入 Snappy/Zstd。
- 当前默认 compression type 是 none；fake codec 用于验证 codec 抽象边界，后续可替换为 Snappy/Zstd。

## 后续优化方向

- 更完整的 Compaction：按 level score 选择文件、限制输出文件大小、控制 grandparent overlap。
- 流式 SSTable Builder：当前 compaction 输入端已流式归并，输出端仍生成一个 vector 后调用 `CreateFromEntries`，后续可改成边归并边写 DataBlock。
- 并发读写：进一步缩短 compaction/manifest 更新期间对版本锁的占用，并引入更系统的长时间压力测试。
- 更细的 fsync 策略：支持批量提交、可配置 durability。
- 真实压缩库：接入 Snappy/Zstd 并做压缩率、CPU、cache 命中率权衡。
