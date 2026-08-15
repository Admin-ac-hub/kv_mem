# Mini LSM-KV Interview Notes

这份文档用于把项目讲成一个完整的存储引擎工程，而不是功能列表。面试时可以按“目标 -> 写入 -> 读取 -> 恢复 -> 优化 -> 限制”的顺序讲。

## 30 秒介绍

我实现了一个 C++17 单机 LSM-Tree KV 存储引擎。写入路径包含 WriteBatch 原子提交、WAL batch 崩溃恢复、SkipList MemTable、immutable MemTable、后台 Flush 到 SSTable 和 Manifest 元数据更新；读取路径包含 MemTable、Bloom Filter、SSTable block index 和 LRU Block Cache；同时实现了 MVCC Snapshot、heap-based MergingIterator 范围扫描、简化 L0/L1/L2 Compaction、CRC32 损坏检测、崩溃恢复、单元测试和 benchmark 统计。

这个项目的重点是把 LevelDB/RocksDB 的核心设计拆成可解释的小模块，验证一个 KV 引擎从写入、持久化、恢复、读优化到一致性读视图的完整闭环。

## 可以重点展开的设计点

### 1. 为什么用 LSM-Tree

LSM 的核心取舍是把随机写转成顺序写。写入先追加 WAL，再写内存里的 MemTable；MemTable 满了以后切成 immutable MemTable，由后台线程批量写成有序 SSTable。这样写路径简单且顺序化，但读路径可能要查多个 SSTable，所以需要 Bloom Filter、block index、Block Cache 和 Compaction 来控制读放大。

### 2. 写入链路

`DB::Put/Delete` 都复用 `DB::Write(const WriteBatch&)`，核心顺序是：

```text
Validate batch
  -> assign consecutive sequence numbers
  -> WAL::AppendBatch + fsync
  -> active MemTable Put/Delete
  -> maybe switch active MemTable to immutable
  -> background Flush immutable MemTable to SSTable
  -> append Manifest VersionEdit
```

这里 WAL 必须先于 MemTable，因为进程崩溃后内存会丢失，只能靠 WAL 重放恢复最近还没 flush 的数据。`WriteBatch` 把一组 Put/Delete 写成一个 WAL batch record，恢复时要么按 batch 重放，要么在损坏时返回明确错误，避免 batch 内部分提交造成状态不一致。

MemTable 达到阈值后，前台线程会切换出一个 immutable MemTable，并打开新的 WAL 继续接收写入；后台线程负责把 immutable MemTable flush 成 SSTable。Manifest fsync 成功后才更新当前版本并清理旧 WAL，这样重启时可以通过 CURRENT/MANIFEST 和 live WAL 恢复出一致状态。

### 3. 读取链路

`DB::Get` 的顺序是：

```text
active MemTable
  -> immutable MemTables from newest to oldest
  -> newest SSTable ... oldest SSTable
  -> Bloom Filter
  -> in-memory block index
  -> Block Cache
  -> DataBlock
```

查 SSTable 时从新到旧，因为新文件里的版本更新。Delete 会写 tombstone，如果新 MemTable 或 SSTable 里读到 tombstone，就返回 NotFound，并停止继续查旧 SSTable，避免旧值复活。

### 4. MVCC 和 Snapshot

每次 `Put/Delete` 都分配全局递增 sequence number，并把 sequence 写入 WAL、MemTable、SSTable 和 Manifest。普通读使用当前最新 sequence；Snapshot 捕获创建时的 sequence，之后的 `Get` 和 `Iterator` 只读取 `sequence <= snapshot_sequence` 的最新可见版本。

这个设计解决的是“读视图稳定性”：即使 snapshot 创建后同一个 key 被更新或删除，snapshot 仍能读到创建时可见的版本。Compaction 时也要感知活跃 snapshot，保留 snapshot 仍可能读到的历史版本。

### 5. Iterator 范围扫描

`NewIterator(ReadOptions)` 会为当前读视图创建多个 child iterator：SSTable iterator、immutable MemTable iterator 和 active MemTable iterator。外层 `MergingInternalIterator` 用 heap 做 lazy merge，每次只推进当前最小 key 对应的 child，而不是先把全量可见数据 materialize 到一个大 vector。

Iterator 在 read sequence 下处理同一个 user key 的多个版本：选择 `sequence <= read_sequence` 的最新非删除版本输出，跳过更旧版本和 tombstone。这样既能支持 `SeekToFirst`、`Seek(target)`、`Next`、`Valid`、`key`、`value` 和 `status`，也更接近生产 LSM 的在线归并扫描路径。

### 6. SSTable 文件格式

SSTable 采用：

```text
DataBlock...
IndexBlock
FilterBlock
Footer
```

DataBlock 存真实 key/value/sequence/deleted，并带 CRC32。IndexBlock 存每个 DataBlock 的最大 internal key、offset 和 size。FilterBlock 仍按 user key 构建 Bloom Filter。Footer 存 index/filter 的位置和 magic number，打开文件时先读 Footer，再加载 index/filter。

### 7. 崩溃恢复

`DB::Open` 的恢复流程：

```text
Read CURRENT
  -> replay Manifest VersionEdits
  -> open live SSTables
  -> replay live WAL files
  -> rebuild active MemTable
```

Manifest 记录当前 WAL 编号、下一个文件编号、last sequence 和 SSTable level/range/size。Manifest 本身带 checksum，如果 checksum mismatch，返回 Corruption。WAL record 也带 CRC32，遇到坏 record 不静默忽略。

Manifest 还记录 `last_sequence`。如果最新写入还在 WAL 中，启动时 replay WAL 会恢复对应 sequence，并把内存里的 last sequence 推进到 WAL 中的最大值，保证重启后不会重复发号。

### 8. Compaction

当前实现的是简化 L0/L1/L2 leveled compaction。优先选择所有 L0 SSTable，并把它们与 key range 重叠的 L1 文件一起合并，输出到 L1；如果没有 L0 输入，则选择 L1 文件和重叠 L2 文件合并，输出到 L2。这个策略没有实现 LevelDB/RocksDB 完整的 level score、grandparent overlap 控制和多输出文件切分，但能展示 leveled compaction 的核心取舍。

Compaction 输入端通过 SSTable iterator 和 heap 做流式归并，按 user key 合并多版本记录。没有活跃 snapshot 时，每个 key 只保留最新版本；L0 合并到 L1 时仍保留最新 tombstone，只有合并到最底层 L2、确认没有更旧层数据可能复活时才丢弃。有活跃 snapshot 时，保留 `sequence > min_snapshot_sequence` 的版本，以及一个 `sequence <= min_snapshot_sequence` 的边界版本，避免 snapshot 被 compaction 破坏。

这个实现能展示 Compaction 的本质：减少 SSTable 数量、消除旧版本、控制读放大，同时在 MVCC 场景下不能随意删除仍可能被 Snapshot 看到的历史版本。

## 面试官可能追问

### 为什么 Manifest 要单独存在

因为 SSTable 是不可变文件，但“当前哪些 SSTable 属于数据库版本”是会变化的。Manifest 把版本元数据和数据文件解耦，启动时不用靠猜目录状态来判断当前版本。

### Bloom Filter 可能误判怎么办

Bloom Filter 只会 false positive，不会 false negative。它说不存在时可以直接跳过 SSTable；它说可能存在时还要继续查 index 和 DataBlock。

### 为什么需要 Block Cache

SSTable 是按 block 读取的。热点 key 通常会落在少量 block 上，Block Cache 可以避免反复读磁盘和反复做 block 解码。

### Snapshot 为什么需要 sequence number

因为 Snapshot 本质上是一个逻辑时间点。只要每条写入都有递增 sequence，读取时选择不超过 snapshot sequence 的最新版本，就能得到稳定的一致性读视图。

### Compaction 为什么不能直接删旧版本

如果有活跃 snapshot，旧版本可能仍然对这个 snapshot 可见。直接删除会导致 snapshot 读不到创建时应该看到的数据。因此 compaction 必须先看最老活跃 snapshot 的 sequence，再决定哪些历史版本可以回收。

### 当前并发模型是什么

当前实现没有追求 lock-free，而是把不同状态拆成几类锁来缩短临界区：`write_mu_` 串行化 WAL append 和 sequence 分配，保证写入顺序；`version_mu_` 保护 Manifest、SSTable 版本和 snapshot 列表；`memtable_mu_` 用 shared/exclusive lock 区分读 MemTable 和写 MemTable；`bg_mu_` 配合 condition variable 管理后台 flush/compaction 的唤醒和停止。

读路径会短暂拿版本锁复制当前 MemTable/SSTable 引用，然后释放 DB 全局状态锁再做实际查询，避免读请求长期阻塞后台 flush 或版本更新。

### 和 LevelDB 还有哪些差距

当前限制和与 LevelDB 的主要差距统一维护在 [README](../README.md#当前限制)，避免多份功能清单随实现演进后互相矛盾。

## 简历项目描述

可以直接使用这段：

```text
Mini LSM-KV：基于 C++17 实现单机 LSM-Tree KV 存储引擎，支持 WriteBatch 原子提交、WAL batch 崩溃恢复、SkipList MemTable、immutable MemTable、后台 Flush、分块 SSTable、MVCC Snapshot、lazy MergingIterator 范围扫描、Bloom Filter、LRU Block Cache、追加式 Manifest VersionEdit 与简化 L0/L1/L2 Compaction；为 WAL、SSTable DataBlock、Manifest 增加 CRC32 校验，并提供单元测试和 benchmark 统计 QPS、延迟、范围扫描吞吐、缓存命中、Bloom 过滤和磁盘占用。
```

## 可讲的优化路径

项目 roadmap 统一维护在 [README 的“后续优化方向”](../README.md#后续优化方向)。面试时从中选择一个方向，说明当前瓶颈、实现方案和可量化的验证指标。
