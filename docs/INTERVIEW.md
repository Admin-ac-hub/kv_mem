# Mini LSM-KV Interview Notes

这份文档用于把项目讲成一个完整的存储引擎工程，而不是功能列表。面试时可以按“目标 -> 写入 -> 读取 -> 恢复 -> 优化 -> 限制”的顺序讲。

## 30 秒介绍

我实现了一个 C++17 单机 LSM-Tree KV 存储引擎。写入路径包含 WAL、SkipList MemTable、Flush 到 SSTable 和 Manifest 元数据更新；读取路径包含 MemTable、Bloom Filter、SSTable block index 和 LRU Block Cache；同时实现了 MVCC Snapshot、Iterator 范围扫描、Level-0 Compaction、CRC32 损坏检测、崩溃恢复、单元测试和 benchmark 统计。

这个项目的重点是把 LevelDB/RocksDB 的核心设计拆成可解释的小模块，验证一个 KV 引擎从写入、持久化、恢复、读优化到一致性读视图的完整闭环。

## 可以重点展开的设计点

### 1. 为什么用 LSM-Tree

LSM 的核心取舍是把随机写转成顺序写。写入先追加 WAL，再写内存里的 MemTable；MemTable 满了以后批量写成有序 SSTable。这样写路径简单且顺序化，但读路径可能要查多个 SSTable，所以需要 Bloom Filter、block index、Block Cache 和 Compaction 来控制读放大。

### 2. 写入链路

`DB::Put` 的顺序是：

```text
Validate key/value
  -> WAL::AppendPut
  -> MemTable::Put
  -> MaybeFlushMemTable
```

这里 WAL 必须先于 MemTable，因为进程崩溃后内存会丢失，只能靠 WAL 重放恢复最近还没 flush 的数据。Flush 成功后会生成新的 SSTable，切换到新的 WAL，再更新 Manifest。

### 3. 读取链路

`DB::Get` 的顺序是：

```text
MemTable
  -> newest SSTable ... oldest SSTable
  -> Bloom Filter
  -> in-memory block index
  -> Block Cache
  -> DataBlock
```

查 SSTable 时从新到旧，因为新文件里的版本更新。Delete 会写 tombstone，如果新 SSTable 里读到 tombstone，就返回 NotFound，并停止继续查旧 SSTable，避免旧值复活。

### 4. MVCC 和 Snapshot

每次 `Put/Delete` 都分配全局递增 sequence number，并把 sequence 写入 WAL、MemTable、SSTable 和 Manifest。普通读使用当前最新 sequence；Snapshot 捕获创建时的 sequence，之后的 `Get` 和 `Iterator` 只读取 `sequence <= snapshot_sequence` 的最新版本。

这个设计解决的是“读视图稳定性”：即使 snapshot 创建后同一个 key 被更新或删除，snapshot 仍能读到创建时可见的版本。Compaction 时也要感知活跃 snapshot，保留 snapshot 仍可能读到的历史版本。

### 5. Iterator 范围扫描

`NewIterator(ReadOptions)` 会在指定读视图下 materialize 当前可见数据：对同一个 user key 选择 sequence 不超过读视图的最新非删除版本，然后按 key 升序输出。它支持 `SeekToFirst`、`Seek(target)`、`Next`、`Valid`、`key`、`value` 和 `status`。

这里没有实现复杂的多路归并在线迭代，而是先选择简单稳定的 materialized iterator，重点保证 Snapshot 语义和 tombstone 过滤正确。

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
Load Manifest
  -> Open SSTables
  -> Replay current WAL
  -> Rebuild MemTable
```

Manifest 记录当前 WAL 编号、下一个文件编号和 SSTable 列表。Manifest 本身带 checksum，如果 checksum mismatch，返回 Corruption。WAL record 也带 CRC32，遇到坏 record 不静默忽略。

Manifest 还记录 `last_sequence`。如果最新写入还在 WAL 中，启动时 replay WAL 会恢复对应 sequence，并把内存里的 last sequence 推进到 WAL 中的最大值，保证重启后不会重复发号。

### 8. Compaction

当前实现的是 Level-0 全量 Compaction。它会读取所有 SSTable，按 user key 合并多版本记录。没有活跃 snapshot 时，每个 key 只保留最新非删除版本，最新版本是 tombstone 就整体清理；有活跃 snapshot 时，保留 `sequence > min_snapshot_sequence` 的版本，以及一个 `sequence <= min_snapshot_sequence` 的边界版本，避免 snapshot 被 compaction 破坏。

这个实现简单但能展示 Compaction 的本质：减少 SSTable 数量、消除旧版本、控制读放大。

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

当前 `DB` 使用一个 mutex 保护公共操作，保证实现简单和状态一致。进一步优化可以把读路径改成读写锁，引入 immutable memtable 和后台 flush/compaction，减少写入阻塞。

### 和 LevelDB 还有哪些差距

主要差距包括多层 Compaction、batch write、压缩、后台线程、细粒度锁、manifest edit log、在线多路归并 Iterator 和更完整的 crash consistency。

## 简历项目描述

可以直接使用这段：

```text
Mini LSM-KV：基于 C++17 实现单机 LSM-Tree KV 存储引擎，支持 WAL 崩溃恢复、SkipList MemTable、分块 SSTable、MVCC Snapshot、Iterator 范围扫描、Bloom Filter、LRU Block Cache、Manifest 元数据管理与 Level-0 Compaction；为 WAL、SSTable DataBlock、Manifest 增加 CRC32 校验，提供单元测试和 benchmark 统计 QPS、延迟、扫描吞吐、缓存命中、Bloom 过滤和磁盘占用。
```

## 可讲的优化路径

- 把 Level-0 全量 Compaction 改成多层 Compaction，降低单次合并成本。
- 将 MemTable flush 改成 immutable memtable + 后台线程。
- 增加 write batch，把多次 WAL append 合并成一次 group commit。
- 对 SSTable block 增加压缩和 prefix compression。
- 把 materialized Iterator 升级为在线多路归并 Iterator，减少大范围扫描内存占用。
