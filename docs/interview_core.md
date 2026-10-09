# Mini LSM-KV 面试核心素材

> **怎么用这份素材**
>
> 每题固定三层：**主干 → 函数名+行号 → 边界**。
> 面试时**先答第 1 句**，等面试官追问再给第 2、3 句。**不要一次全倒出来。**
>
> 行号用于你自己复习核对，面试时**说函数名就够**，不用报行号。
>
> 记不住全部 30 题不要紧 —— 先吃透 A / B / D / G 四组（全链路、写入、MVCC、Compaction），
> 这四组覆盖了面试 70% 的追问。
>
> **口径说明**：本材料是纯 LSM 口径，通篇不含向量检索 / HNSW。
> 仓库里的 `hnsw_*.cc`、`vector_value.cc` 属非主线，被直接问到再用 30 秒兜底话术。

---

## 目录

- **A. 架构与全链路**（4 题）
- **B. 写入路径与持久化**（5 题）
- **C. 读取路径**（5 题）
- **D. 时间维度：MVCC 与 Snapshot**（4 题）
- **E. 范围扫描**（3 题）
- **F. Manifest 与崩溃恢复**（4 题）
- **G. Compaction 与并发**（5 题）
- **H. 易错清单**（面试前 5 分钟扫一遍）

---

# A. 架构与全链路

## A1. 一句话介绍这个存储引擎

**答（三句）：**

1. C++17 单机 LSM-Tree KV 存储引擎，写入走 `WriteBatch` 原子提交 → WAL fsync → SkipList MemTable → 后台 Flush 成 SSTable；读取走 MemTable → Bloom Filter → Block Index → LRU Block Cache → DataBlock。
2. 在此之上实现了 MVCC Snapshot、heap-based MergingIterator 范围扫描、追加式 Manifest、简化 L0/L1/L2 Compaction，以及 WAL / DataBlock / Manifest 三处 CRC32 校验。
3. **边界**：单进程；Compaction 没有 level score / grandparent overlap / 多文件输出切分；每条 batch 一次 fsync，没有 group commit。

**追问预判：**
- 多少代码量？→ LSM 主线约 4.2k 行：`db.cc` 1916、`sstable.cpp` 936、`manifest.cpp` 566、`wal.cc` 273、`skiplist.cc` 133
- 有测试吗？→ `test/kv_test.cc` 确定性单测 + `stress/` 多线程随机对拍 + ASan/UBSan/TSan CI

---

## A2. 一次写入的完整链路

**答（三句）：**

1. `DB::Put/Delete` 统一转成 `WriteBatch`，全部汇到 `DB::Write`（`db.cc:436`）。
2. 顺序是：校验 → 取 `manifest_->LastSequence() + 1` 作起始 sequence → `wal_->AppendBatch` → 逐条写 `active_memtable_` → `MaybeFlushMemTable`。
3. **关键点**：WAL 那一步在 MemTable 之前（`db.cc:487` 早于 `db.cc:498`）；MemTable 满了只做「切换 + 通知后台」，真正的落盘在后台线程。

**关键文件：**
`db.cc:436/487/516/1591/1630/1730` · `wal.cc:64` · `sstable.cpp:495`

**函数名怎么记（别全背，分三层）：**

| 层 | 函数 | 要求 |
|---|---|---|
| **必记（6 个，链路骨架）** | `DB::Write` → `WALWriter::AppendBatch` → `MemTable::Put` → `MaybeFlushMemTable` → `FlushMemTable` → `BackgroundWorkerLoop` | **条件反射**，卡一个就露馅 |
| **追问时要说得出（4 个）** | `FlushImmutableMemTable`、`SSTable::CreateFromEntries`、`SaveManifest`、`RemoveObsoleteWALFiles` | 被问到能答 |
| **可以不记（用功能描述代替）** | `ValidateKey`、`AllocateFileNumber`、`SetLastSequence`、`FsyncFile` | 见下方替代说法 |

第三层的替代说法：
- `ValidateKey` → "校验 key 不含制表符/换行/NUL"
- `AllocateFileNumber` → "从 Manifest 拿下一个文件号"
- `SetLastSequence` → "推进已分配的最大 sequence"
- `FsyncFile` → "对 WAL 文件做一次 fsync"

**练法（由粗到细）**：先只背「必记」那 6 个，把骨架串顺；串顺了再把第二层插进去；
第三层永远用功能描述，不刻意背。

**卡壳怎么办**：想不起函数名时**立刻用功能描述顶上，继续往下讲**。
"后台线程把不可变 MemTable 写成 SSTable" 比 "然后……那个函数……叫什么来着" 好十倍。
**卡壳比说错更致命。**

---

## A3. 一次点查（Get）的完整链路

**答（三句）：**

1. 先拿读视图：`DB::Get`（`db.cc:538`）在 `version_mu_` 下用 `ReadSequence`（`db.cc:1815`）定出 `read_sequence`，然后把 active MemTable、immutable 队列、SSTable 列表**各复制一份 `shared_ptr` 出来**，随即释放锁（`db.cc:551-560`）。
2. 查找顺序：active MemTable（`db.cc:568`）→ immutable 从新到旧（`db.cc:578`）→ SSTable 从新到旧（`db.cc:596`）。MemTable 命中即返回；SSTable 侧要遍历完再比 sequence。
3. **关键点**：读路径**不长时间持有全局锁**；SSTable 用 `shared_ptr` pin 住，所以 compaction 并发删旧文件时，已在进行的读仍安全。

**关键文件：**
`db.cc:538/551/568/578/596/604` · `memtable.cc:55` · `skiplist.cc:95` · `sstable.cpp:621/827`

**追问预判：**
- MemTable 命中是 O(?) → SkipList 期望 O(log n)（`skiplist.cc:95`）
- 为什么 immutable 要**从新到旧**？→ 越靠前的 immutable 越新，新版本优先
- SSTable 为什么不能"找到就返回"？→ 见 C5，文件顺序不代表版本新旧
- 命中 tombstone 怎么办？→ 立刻返回 NotFound 并**停止继续查旧文件**（`db.cc:570`），防止旧值复活

---

## A4. 为什么用 LSM-Tree

**答（三句）：**

1. 核心取舍：**把随机写换成顺序写**。传统 B+ 树原地更新，每次写都要找页、改页、写回，是随机写；LSM 只往文件末尾追加，是顺序写。
2. 代价是**读放大**：同一个 key 的多个版本散在不同文件里，读要查多处。所以整套读优化（Bloom / Index / Cache / Compaction）本质都是在还这笔债。
3. **边界**：这个取舍在写多读少、且能接受后台写放大时才划算。纯读密集、要求点查低延迟的场景，B+ 树更合适。

**追问预判：**
- 写放大在哪？→ Compaction 反复重写数据；本项目里分层退化成全量重写，写放大 ≈ 64×（见 G5）
- 读放大怎么量化？→ 最坏要查 active + N 个 immutable + M 个 SSTable
- 空间放大？→ 旧版本 + tombstone 在 compaction 前一直占空间

---

# B. 写入路径与持久化

## B1. WAL 记录格式，以及为什么必须先于 MemTable

**答（三句）：**

1. 一条 batch 记录 = `magic(4) | type(1) | sequence(8) | count(4) | payload_size(4) | crc32(4) | payload`。magic 是 `kWALBatchMagic = 0x41574c4b`（`wal.cc:12`），头（不含 magic）17 字节（`wal.cc:82-85`）。
2. CRC32 覆盖「17 字节头 + payload」（`wal.cc:87-89`），写完 `flush` + `FsyncFile`（`wal.cc:100-105`）。
3. **为什么必须先于 MemTable**：崩溃时内存全丢，只有落盘的 WAL 能重放。`DB::Write` 里 `AppendBatch`（`db.cc:487`）在写 MemTable（`db.cc:498`）之前 —— 顺序反了就会出现"内存有、磁盘没有"的窗口。

**追问预判：**
- 为什么整条记录一次 CRC 而不是每字段一个？→ 校验的是"这条记录完整且没被写坏"，粒度对齐重放单位
- WAL 文件什么时候能删？→ 对应数据进了 SSTable 且 Manifest 落盘之后（`RemoveObsoleteWALFiles` `db.cc:1874`）
- WAL 和 Manifest 谁先写？→ MemTable 切换时**先**写 Manifest 记录新 WAL 号，再切（`db.cc:1608`），保证重启能找到新 WAL

---

## B2. WriteBatch 的原子性怎么保证

**答（三句）：**

1. batch 内每个操作编码为 `type(1) | key_size(4) | value_size(4) | key | value`（`WriteBatch::Encode` `write_batch.cc:51`），整批作为**一条** WAL 记录写入。
2. 恢复时按记录整体重放：`WALReader::ReadNext` 先验 CRC，再 `WriteBatch::Decode`，并**核对 `batch.Count()` 与头里的 count 一致**（`wal.cc:243`）。
3. **边界**：所谓原子性来自"一条记录 + 整体 CRC"，**不是**文件 rename 或两阶段提交。记录被写坏 → 尾部截断（B3）；记录完整但内部字段非法 → 返回 `Corruption`，不静默忽略。

**追问预判：**
- batch 内 sequence 怎么分？→ 连续递增，`++sequence`（`db.cc:513`），整批共用一个起始值
- 半个 batch 会被恢复吗？→ 不会。要么整条 CRC 通过整体重放，要么这条被当作 torn tail 截掉
- `Delete` 带 value 会怎样？→ `Decode` 直接报 `Corruption("delete operation carries a value")`（`write_batch.cc:93`）

---

## B3. MemTable 用什么结构，为什么

**答（三句）：**

1. `SkipList`（`skiplist.h:15`），`kMaxLevel = 12`、`kBranching = 0.5`（`skiplist.h:49-50`），节点按 `internal_key` 排序（`skiplist.cc:68`）。
2. 选它是因为**天然有序**：`Entries()` 直接顺序遍历底层链表就能导出全序（`skiplist.cc:111`），范围扫描和 Flush 都不需要额外排序。
3. **边界**：LevelDB 用 SkipList 是为了**无锁读**；本项目读侧仍走 `memtable_mu_`（shared_mutex），所以这里的收益只剩「有序 + 实现简单 + 不用旋转」，**并发收益没有兑现**。这是个诚实的取舍点。

**追问预判：**
- 为什么不用 `std::map`？→ 也能用，但红黑树旋转 + 指针跳转多，顺序遍历局部性差；SkipList 底层是链表，插入不用再平衡
- 为什么不用哈希表？→ 哈希表无序，导出全序要额外排序，Flush 和 Iterator 都要多一遍
- 随机层数怎么定？→ `RandomLevel()`（`skiplist.cc:32`），`p = 0.5` 每层，期望层数 O(log n)

---

## B4. MemTable 满了之后发生什么

**答（三句）：**

1. `MaybeFlushMemTable`（`db.cc:1584`）判 `Size() >= memtable_entries_limit`（默认 1024，`db.h:33`），触发 `FlushMemTable`（`db.cc:1591`）。
2. `FlushMemTable` 只做**切换**：分配新 WAL 号 → 开新 WAL → **先 `SaveManifest`**（`db.cc:1608`）→ 关旧 WAL → 旧 MemTable 入 immutable 队列 → 换新 active MemTable → `notify_one`（`db.cc:1626`）。
3. 落盘在后台线程：`BackgroundWorkerLoop`（`db.cc:1630`）取队首 immutable，调 `FlushImmutableMemTable`（`db.cc:1730`）→ `SSTable::CreateFromEntries`，成功后追加 Manifest 并 `pop_front`（`db.cc:1703`）。

**追问预判：**
- 前台会被落盘阻塞吗？→ 不会。前台只切换，写完新 WAL 就继续收写入
- 为什么先写 Manifest 再切 MemTable？→ 崩溃后重启能凭 Manifest 里的 WAL 号找到新 WAL；反过来会丢 WAL 号
- 一次能刷几个 immutable？→ 后台循环 `while(true)` 一路刷到队列空（`db.cc:1640`）
- 写失败会怎样？→ `SetBackgroundErrorLocked`（`db.cc:1777`）记下第一个错误，后续 `Write` 直接返回它（`db.cc:443`）

---

## B5. 一条 fsync 的代价，以及为什么没有 group commit

**答（三句）：**

1. 每条 batch 结尾一次 `FsyncFile`（`wal.cc:105`），且 `DB::Write` 全程持 `write_mu_`（`db.cc:437`）—— **写路径完全串行**。
2. 所以写入吞吐上限 = `1 / (fsync 延迟 + 编码开销)`。实测 6000×768、batch=256 配置下 **316.93 条/s**（来源 `docs/REVIEW_FIXES_20260916.md:74`）。
3. **这是主动认领的取舍**：换取"单条 batch 落盘即持久"的强语义和实现简单。要提速就得做 writer queue + group commit（合并多个 batch 一次 fsync），属后续方向。

**追问预判：**
- group commit 怎么做？→ 一组并发写入共享一次 fsync：先入队，由单个线程收集同批请求，一次 fsync 后统一返回
- 代价是什么？→ 延迟变差（要等同批凑齐或超时），语义从"立即持久"变成"组内持久"
- 为什么现在不做？→ 先要正确性闭环；吞吐是可量化的后续优化项

---

# C. 读取路径

## C1. SSTable 的文件格式

**答（三句）：**

1. 布局：`DataBlock... | IndexBlock | FilterBlock | Footer`。Footer 40 字节（`sstable.cpp:23`），存 `index_offset / index_size / filter_offset / filter_size / magic`，magic 是 `0x4b565354424c4b33`（`sstable.cpp:22`）。
2. DataBlock 存按 `user_key + sequence` 排序的版本化记录，用 restart interval = 16（`sstable.cpp:26`）做前缀压缩；每个 block 尾部有 5 字节 trailer：压缩类型(1) + CRC32(4)（`sstable.cpp:27`）。
3. 打开文件时 `LoadIndex`（`sstable.cpp:710`）只读 Footer + IndexBlock + FilterBlock 进内存，DataBlock 按需读。

**追问预判：**
- IndexBlock 里存什么？→ 每个 DataBlock 的**最大 internal key** + offset + size，用 `lower_bound` 定位候选 block
- 为什么 Index 常驻内存而 DataBlock 不？→ Index 小（一个 block 一条），DataBlock 大且热点集中，交给 Block Cache
- 打开一个 SSTable 读几次盘？→ 3 次（Footer、Index、Filter），之后按需读 block

---

## C2. 点查在 block 内怎么定位

**答（三句）：**

1. 先二分 restart points 找到不超过目标的最后一个 restart 点（`sstable.cpp:875-892`），`RestartKey` 解出该点的 key 再比较。
2. 从该 restart point **顺序解码**向前走，直到 `key` 命中或已超过目标（`sstable.cpp:897-918`）。
3. **边界**：restart 点的 key 是完整存储的（`shared = 0`，`sstable.cpp:94-97`），所以能直接比较；非 restart 点只存 `shared/unshared` 前缀差，**必须先解码前一条才能还原**。所以不能对任意位置二分。

**追问预判：**
- 为什么 restart interval 是 16？→ 折中：间隔越小，随机访问越快、空间越大；LevelDB 同值
- 计数在哪？→ `restart_seek_count_`（`sstable.cpp:873`），`Stats()` 里输出
- 前缀压缩省多少？→ 省掉与上一条相同的 key 前缀；单调递增的 key（如带时间戳）收益最大

---

## C3. Bloom Filter 的作用与误判

**答（三句）：**

1. 构建时按 **user key** 去重后建（`sstable.cpp:504-508`），位数组大小 = `keys × bits_per_key`（默认 10），哈希个数 = `bits_per_key × 0.69`（`bloom_filter.cc:12-16`）。
2. 查 `SSTable::Get` 时**第一件事**就是 `filter_.MayContain(key)`（`sstable.cpp:626`），返回 false 就直接 `NotFound`，连 Index 都不查，计数进 `bloom_filtered_count_`。
3. **边界**：只有 **false positive**，没有 false negative。说"不存在"可以直接跳过；说"可能存在"还必须继续查 Index 和 DataBlock。

**追问预判：**
- 为什么是 0.69 而不是 ln2？→ `ln 2 ≈ 0.693`，是"位数组大小固定时误判率最小"的最优哈希个数
- 误判率大概多少？→ 10 bits/key 时约 1%，可用 `bloom_filtered` 与 `block_reads` 的比值反推
- FilterBlock 也要 CRC 吗？→ 不需要，它只影响性能不影响正确性（漏读会退化成正常路径）

---

## C4. Block Cache 为什么需要

**答（三句）：**

1. SSTable 按 block 读（默认 4096 字节，`db.h:35`），热点 key 通常集中在少量 block 上。
2. 实现是 **LRU**：`std::list` 存数据、`std::unordered_map` 存 `(file_number, block_offset) → list 迭代器`（`block_cache.h:41-46`）。命中时 `splice` 到队首（`block_cache.cc:23`），插入超容量从队尾淘汰（`block_cache.cc:45-48`）。
3. **边界**：默认容量只有 64 个 block（`db.h:36`），且整个 cache 一把 `std::mutex`（`block_cache.h:44`）—— 高并发下会成为争用点，可换分片锁。

**追问预判：**
- 为什么用 `std::list` 不用 `std::deque`？→ LRU 要 O(1) 把任意节点挪到队首，`list::splice` 是 O(1)；`deque` 中间移动是 O(n)
- 缓存的 key 为什么是 `(file_number, offset)` 而不是 key？→ 缓存的是**解码前的原始 block 字节**，一个 block 含多条记录，按文件位置索引才自然
- 为什么缓存原始字节而不是解码后的 `vector<VersionedEntry>`？→ 原始字节紧凑、可共享；解码后的结构每读一次都要构造

---

## C5. Get 为什么不能按文件顺序取（易错题）

**答（三句）：**

1. 直觉上"新文件版本更新"，但**并发下不成立**：一次 flush 可能先于一个正在构建的 compaction 发布，导致文件号小的表里反而有更新的版本（`db.cc:591-592` 的注释就是这个）。
2. 所以 SSTable 侧改为**按 sequence 取最大**：遍历所有表，只在 `sequence > newest_sequence` 时更新结果（`db.cc:604`），并记录命中的是不是 tombstone（`db.cc:607`）。
3. **边界**：有个提前退出 —— 命中 `sequence == read_sequence` 就 `break`（`db.cc:609`），因为不可能有比读视图更新的可见版本。代价是**读放大**：点查要扫所有 SSTable，除非这次提前退出。

**追问预判：**
- MemTable 侧为什么没这个问题？→ MemTable 有明确的新旧顺序（active → immutable 由新到旧），且 `Get` 直接返回第一个命中
- 这个修复的测试在哪？→ `TestGetUsesSequenceAcrossConcurrentCompactionAndFlush`
- 代价是什么？→ 一次点查最坏读 M 个 SSTable；正常路径靠 Bloom 把它们挡掉

---

# D. 时间维度：MVCC 与 Snapshot

## D1. sequence number 是怎么用的（核心题）

**答（三句）：**

1. `SequenceNumber` 就是 `uint64_t`（`types.h:8`），每次写入从 `manifest_->LastSequence() + 1` 起分配，batch 内连续递增（`db.cc:486/513`）。
2. **一个数字同时管三件事**：① 版本可见性 —— 只读 `sequence <= read_sequence` 的最新版本；② Snapshot —— 快照就是记下当时的 `last_sequence`；③ 版本回收 —— compaction 靠它判断哪些旧版本还能丢。
3. **边界**：它是**唯一的逻辑时钟**，不依赖墙钟时间，所以不受时钟回拨影响；重启后靠 Manifest 的 `last_sequence` + WAL 重放推进，保证不重发号（`db.cc:1573`）。

**追问预判：**
- 为什么用递增整数而不是时间戳？→ 时间戳会回拨、有精度问题；单调整数天然给出全序
- sequence 什么时候写进文件？→ WAL 记录头（`wal.cc:83`）、internal key 尾部（`format.cc:78`）、Manifest 的 `last_sequence`
- 会不会溢出？→ uint64，按 100 万次写/s 算要 58 万年

---

## D2. Snapshot 怎么实现

**答（三句）：**

1. `Snapshot` **只存一个 `sequence_`**（`db.h:69-79`），不拷贝任何数据 —— 它是逻辑时间点。
2. `GetSnapshot`（`db.cc:1833`）在锁下取当前 `last_sequence` 造一个对象，挂进 `snapshots_`；`ReadSequence`（`db.cc:1815`）有快照就用快照的 sequence，没有就用最新。
3. **边界**：`ReleaseSnapshot`（`db.cc:1839`）从列表里摘掉。**只有还在 `snapshots_` 里的快照才受保护** —— 忘了 release 会导致 compaction 一直保留旧版本，空间不回收。

**追问预判：**
- 快照之间互相影响吗？→ 不。每个快照独立，读时各用自己的 sequence
- 快照能读到快照创建后的写入吗？→ 不能，只读 `sequence <= snapshot_sequence`
- 为什么用 `vector<unique_ptr<Snapshot>>` 而不是 `shared_ptr`？→ 生命周期由 DB 统一管，返回裸指针，避免引用计数开销

---

## D3. internal key 为什么把 sequence 倒序编码

**答（三句）：**

1. `EncodeInternalKey`（`format.cc:77`）= `user_key + '\0' + 8 字节 (max - sequence)`，**用补码把 sequence 反过来存**。
2. 目的：让同一个 user key 的多个版本在**字节序排序下按 sequence 降序排列** —— 顺序扫描时最新版本排在最前面。
3. **边界**：于是 `Get` 从任意位置向后扫，遇到的**第一个** `sequence <= read_sequence` 的记录就是答案，不需要扫完整个 key 的所有版本。

**追问预判：**
- 中间那个 `'\0'` 干什么？→ 分隔符，保证 `user_key` 是前缀时排序仍然正确（`"a"` 的所有版本排在 `"ab"` 之前）
- 为什么要 `max - sequence` 而不是直接写 sequence？→ 大端写递增数会升序排列，最新版本排最后，反而要扫到底
- 解析回来呢？→ `DecodeInternalKey`（`format.cc:87`），校验倒数第 9 字节是 `'\0'`，再 `max - inverted` 还原

---

## D4. tombstone 与删除语义

**答（三句）：**

1. `Delete` 不删数据，而是写一条 `deleted = true` 的记录（`SkipList::Delete` `skiplist.cc:60`；SSTable 里是 `ValueType::kDelete`，`sstable.h:22`）。
2. 读取时命中 tombstone 就返回 `NotFound` 并**停止继续查更旧的文件**（`db.cc:570`）—— 这是防止已删的 key 被旧值"复活"的关键。
3. **边界**：tombstone 本身也要参与 compaction 的多版本合并，且不能随便丢（见 G2）。

**追问预判：**
- 为什么不在原位置删？→ LSM 不可原地改，且原地删会破坏 MVCC —— 老快照可能还要读旧值
- tombstone 会一直占空间吗？→ 不会，落到最底层且确认无更旧层时会被丢弃
- `Delete` 一个不存在的 key 会报错吗？→ 不会，写 tombstone 是幂等语义，读时返回 NotFound

---

# E. 范围扫描

## E1. MergingIterator 怎么工作

**答（三句）：**

1. `NewIterator`（`db.cc:1850`）为每个 SSTable 建一个 child iterator（`SSTable::NewIterator`），再为每个 MemTable 建一个 `VectorInternalIterator`，全部塞进 `MergingInternalIterator`（`db.cc:110`）。
2. 归并用**最小堆**：`RebuildHeap`（`db.cc:178`）把各 child 的当前位置压堆，`AdvanceToNextVisible`（`db.cc:208`）反复取堆顶、推进、重压。
3. **关键点**：同一个 key 的所有版本一次性从堆里弹完，选 sequence 最大的那个；若是 tombstone 就跳过整组，继续下一组（`db.cc:224`）。

**追问预判：**
- 一次 `Next()` 的代价？→ O(log C)，C 是 child 数
- `Seek` 呢？→ 所有 child 先 `Seek(target)`（`db.cc:123`），再重建堆
- 重复 key 怎么去重？→ `AdvanceChildPastKey`（`db.cc:194`）把该 child 推过这个 key 的所有版本

---

## E2. 堆的比较规则为什么是"反着比"

**答（三句）：**

1. `HeapCompare`（`db.cc:165-176`）写的是：key 大的排后面，key 相同时 sequence **小的**排后面。
2. 因为 `std::priority_queue` 是**最大堆**，`top()` 取的是"最大"的那个 —— 所以规则反着写，`top()` 拿到的才是 key 最小、且 sequence 最大的那一条。
3. **边界**：这个方向和 internal key 的倒序编码（D3）是**一致**的 —— 都是"让最新版本排最前"。两处必须同时正确，否则会读到旧值。

**追问预判：**
- 为什么不直接给一个升序比较器？→ 可以用 `greater` 包装，但语义绕；LevelDB 也是这个写法
- 跨文件同 key 同 sequence 可能吗？→ 不可能，sequence 全局唯一
- 堆里会有重复 child 吗？→ 不会，每个 child 每次最多在堆里出现一次

---

## E3. Iterator 真的"懒"吗（易错题）

**答（三句）：**

1. **SSTable 侧是懒的**：`SSTableInternalIterator` 按 block 加载（`EnsureBlockLoaded` `sstable.cpp:429`），只在需要时读下一个 block，不预读整表。
2. **但 MemTable 侧不是**：`NewIterator` 里对每个 MemTable 调的是 `Entries()`（`db.cc:1859/1862`），**先把整个 MemTable 全量物化成 `vector`**，再交给 `VectorInternalIterator`。
3. **边界**：所以"全程不物化"的说法不成立。好在 MemTable 有上限（默认 1024 条），物化成本可控 —— 但**面试时不要答"完全不物化"**。

**追问预判：**
- 为什么 MemTable 要物化？→ 为了统一到 `InternalIterator` 接口；SkipList 的节点没有现成的迭代器抽象
- 怎么改成真懒？→ 给 SkipList 写一个基于 `next[0]` 链表的迭代器，按需推进
- `DB::CompactUnlocked` 的输出端呢？→ 也是物化的，`std::vector<VersionedEntry> output`（`db.cc:921`）

---

# F. Manifest 与崩溃恢复

## F1. 为什么需要 Manifest 和 CURRENT

**答（三句）：**

1. SSTable 是**不可变文件**，但"当前版本包含哪些 SSTable、每个在第几层"是会变的。Manifest 把版本元数据和数据文件解耦，重启时不用靠猜目录状态。
2. 追加式：每次 `Save` 只写一条差分记录（`add_file` / `delete_file`），由 `BuildEditPayload`（`manifest.cpp:33`）算出与上次的差异；记录头是 `record <size> <crc>\n`（`manifest.cpp:70`）。
3. **边界**：`CURRENT` 是一个只存 MANIFEST 文件名的间接层（`WriteCurrentFile` `manifest.cpp:435`），写时走 `tmp + rename + fsync` 保证原子替换。

**追问预判：**
- Manifest 里都有什么？→ `next_file_number`、`wal_file_number`、`last_sequence`、每个 SSTable 的编号/层级/key range/大小
- 为什么追加而不是每次重写全量？→ 重写要写全部文件列表，追加只写差异，且天然带历史
- Manifest 也会坏吗？→ 会，每条记录带 CRC，校验失败返回 `Corruption`（`manifest.cpp:401`）

---

## F2. 崩溃恢复的完整流程

**答（三句）：**

1. `DB::Open`（`db.cc:303`）依次调 `LoadOrRecoverManifest` → `OpenSSTables` → `Recover`。
2. `LoadOrRecoverManifest`（`db.cc:1237`）：有 Manifest 就 `Manifest::Load`（`manifest.cpp:303`）读 CURRENT → 重放 VersionEdit；**没有 Manifest 就扫目录重建**（`db.cc:1248`，兼容旧库）。
3. `Recover`（`db.cc:1493`）只重放 `number >= manifest_->WALFileNumber()` 的 WAL，逐条 `ReadNext` 后写回 active MemTable，并推进 `last_sequence`（`db.cc:1573`）。

**追问预判：**
- 恢复顺序能换吗？→ 不能。Manifest 决定"哪些 WAL 是活的"，必须先定 WAL 集合
- 未发布的 orphan SSTable 怎么办？→ 打开时只认 Manifest 引用的，多余的忽略（`OpenSSTables` `db.cc:1317`）
- 恢复要多久？→ 与未 flush 的 WAL 量成正比，已 flush 的数据不重放

---

## F3. WAL 尾部写坏了怎么办（torn tail）

**答（三句）：**

1. 崩溃可能留下**写了一半的最后一条记录**。`WALReader::ReadNext` 读到不足一条时，若是 EOF 就走 `TruncateTornTail`（`wal.cc:252`）。
2. 动作是 `resize_file` 截到**最后一条完整记录的偏移**（`wal.cc:257`），然后 fsync 文件 + fsync 目录。
3. **边界**：**只截尾部**。如果坏记录出现在中间（`!eof`），返回 `Corruption` 而不是截断 —— 中间坏说明数据真的坏了，静默截断会丢掉后面所有数据。

**追问预判：**
- Manifest 也有 torn tail 吗？→ 有，`LoadEditLog` 里同样的 `torn_tail` 处理（`manifest.cpp:416`），截到最后一条完整 edit
- 截断会不会丢已提交的数据？→ 会丢那条**没写完**的记录 —— 但它本来就没提交成功（fsync 没返回）
- 为什么还要 fsync 目录？→ 文件大小变更属于目录元数据，不 fsync 目录掉电后可能回退

---

## F4. 恢复后 sequence 怎么接上

**答（三句）：**

1. `last_sequence` 存在 Manifest 里（`manifest.cpp:38`）。启动时先由 Manifest 给出基线，再由 WAL 重放推进。
2. 重放时逐条比对：`if (sequence > manifest_->LastSequence()) SetLastSequence(sequence)`（`db.cc:1573`）。
3. **边界**：这样即使 Manifest 里的 `last_sequence` 落后于 WAL（因为 Manifest 是异步落盘的），重放也能把它顶到真实最大值，保证**重启后不重发号**。

**追问预判：**
- 如果重发号会怎样？→ 同一个 sequence 对应两个不同写入，MVCC 版本序被破坏，快照可能读到错的数据
- 文件号呢？→ 同样处理，`max_seen_file_number + 1` 保证不复用（`db.cc:1526`）
- 为什么 Manifest 的 last_sequence 会落后？→ 它只在 MemTable 切换/Flush/Compaction 时落盘，不跟着每条写入走

---

# G. Compaction 与并发

## G1. Compaction 的策略

**答（三句）：**

1. 触发在 `MaybeCompact`（`db.cc:1783`）：L0 文件数 > 4（`level0_sstable_limit`）**或** L1 总字节 > 64 MiB（`level1_size_limit_bytes`），最多重试 4 轮。
2. 选择在 `CompactUnlocked`（`db.cc:809`）：有 L0 → 取**全部 L0** + key range 重叠的 L1，输出到 L1（`db.cc:845-865`）；没有 L0 → 取 L1 + 重叠 L2，输出到 L2（`db.cc:866-889`）。
3. 归并：输入端用最小堆流式归并（`db.cc:934-960`），同一个 key 的版本收集齐后排序，按规则决定留哪些，最后**整体物化成 `output`**（`db.cc:921`）再写新 SSTable。

**追问预判：**
- 和 LevelDB 差在哪？→ 没有 level score、没有 grandparent overlap 控制、没有多文件输出切分
- 输出为什么只写一个文件？→ 简化实现；生产要做目标文件大小切分
- 发布的原子性？→ 在 `version_mu_` 下换 `sstables_` + `manifest_->SetSSTables` + `SaveManifest`，失败回滚（`db.cc:1158-1165`）

---

## G2. 为什么不能随便删 tombstone 和旧版本（核心题）

**答（三句）：**

1. 没有活跃快照时，规则是：**只保留最新版本**；但如果最新版本是 tombstone，**只有当 `target_level >= 2`（落到最底层）才丢**（`db.cc:969-973`）。
2. 原因：tombstone 的职责是**屏蔽更低层的旧值**。如果它还在 L1 就被丢掉，L2 里那条更旧的值就会重新"浮上来"被读到 —— 已删除的 key 复活。
3. **边界**：只有确认下面再没有更旧的层（L2 是最底层）时，屏蔽职责才消失，这时才能安全丢弃。

**追问预判：**
- 旧版本为什么也不能随便删？→ 有活跃快照时，`sequence > min_snapshot` 之外的版本可能正是某个快照要读的
- 这是"墓碑"这个类比的意思吗？→ 是。墓碑立着是为了挡住后面的东西，墓挖平了才知道不用立了
- 如果层数扩展到 L3/L4 呢？→ 判断条件要从"`target_level >= 2`"改成"`target_level == 最底层`"，硬编码是当前实现的局限

---

## G3. 有活跃 Snapshot 时 Compaction 怎么保留版本

**答（三句）：**

1. 先取 `min_snapshot = MinActiveSnapshotSequence()`（`db.cc:894`，实现 `db.cc:1820`）—— 所有活跃快照里最小的那个 sequence。
2. 保留规则（`db.cc:976-984`）：**保留所有 `sequence > min_snapshot` 的版本**，再额外保留**一个 `sequence <= min_snapshot` 的边界版本**。
3. **边界**：保留"最老快照之前的那一个版本"就够了 —— 因为所有快照都只能看到 `<= 自己 sequence` 的数据，比最老快照还旧的那一版是所有快照共同需要的下界。

**追问预判：**
- 为什么是"最小的"快照而不是最大的？→ 最保守：满足最老的那个，就满足所有更新的
- 没有活跃快照呢？→ `min_snapshot == 0`，走"只留最新版本"的分支（`db.cc:969`）
- 边界版本怎么选的？→ 排序后（sequence 降序）第一个 `<= min_snapshot` 的（`kept_boundary` 标志，`db.cc:976`）

---

## G4. 四把锁（实际五把）的分工

**答（三句）：**

1. `write_mu_`（`db.h:209`）串行化 WAL append + sequence 分配；`version_mu_`（`db.h:211`）保护 Manifest、`sstables_`、`snapshots_`；`memtable_mu_`（`db.h:212`，shared_mutex）区分读/写 MemTable；`bg_mu_`（`db.h:213`）管后台线程唤醒与停止。
2. 实际还有第五把 `compaction_mu_`（`db.h:210`），串行化 compaction，防止 `Compact()` 与后台 `MaybeCompact` 撞车（`db.cc:1784` / `db.cc:805`）。
3. **关键设计**：读路径只短暂持 `version_mu_` 复制 `shared_ptr` 就释放（`db.cc:551-560`），SSTable I/O 不占全局锁；Compaction 删旧文件时，已开始的读靠 `shared_ptr` pin 住文件继续完成。

**追问预判：**
- 为什么 MemTable 用 shared_mutex 而别的用 mutex？→ MemTable 读多写少，读侧共享锁能并发
- 有死锁风险吗？→ 锁序固定：`write_mu_` → `version_mu_` → `memtable_mu_`；`Close` 里也是这个顺序（`db.cc:376-377`）
- 为什么不用读写锁保护 `sstables_`？→ 写路径（发布新版本）本身要串行且要和 Manifest 一起改，普通 mutex 更简单

---

## G5. 这个项目有什么已知缺陷（主动认领）

**答（三句）：**

1. **分层 compaction 退化为全量重写**：L1 / L2 各**永远只有 1 个文件且覆盖全 key range**（`db.cc:845-889`），所以每次 L0 compaction 都会把整个 L1 重写一遍。
2. 写放大 ≈ `level1_size_limit_bytes / (2 × 一次 L0 引入量)`，按 64 MiB 估算约 **64×**，且**随 limit 线性增长** —— 规模化站不住。
3. 另外两点：输出端是物化的（`db.cc:921`），`INTERVIEW.md` 说的"流式归并"只对输入端成立；每条 batch 一次 fsync、写路径全串行，无 group commit（B5）。

**追问预判：**
- 怎么修？→ 按 level score + 文件 overlap 选输入（Compaction Picker），加目标文件大小切分，让 L1 有多个不重叠文件
- 为什么当初这么做？→ 先用最小实现把"多版本合并 + tombstone 语义"跑通，分层策略是可替换的外围
- 还有什么？→ 单进程、不支持多实例；`KV_ENABLE_RECALL_REGRESSION` 只在 CI Release 启用

---

# H. 易错清单（面试前扫一遍）

| ❌ 别说 | ✅ 应该说 |
|---|---|
| "LSM 写入快，没有代价" | 代价是**读放大**；Bloom / Index / Cache / Compaction 都是在还这笔债 |
| "先写 MemTable 再写 WAL" | **WAL 必须先于 MemTable**（`db.cc:487` 早于 `:498`），否则崩溃丢数据 |
| "WriteBatch 靠事务保证原子性" | 靠**一条 WAL 记录 + 整体 CRC**：要么整条通过整体重放，要么尾部截断 |
| "sequence 就是个版本号" | 它是**唯一逻辑时钟**，同时管版本可见性、Snapshot、版本回收 |
| "Snapshot 拷贝了一份数据" | Snapshot **只存一个 sequence**（`db.h:76`），是逻辑时间点 |
| "Get 按文件从新到旧，找到就返回" | 必须**按 sequence 取最大**（`db.cc:604`）—— compaction 可能后于 flush 发布 |
| "Bloom Filter 判断是准的" | 只有 **false positive**，没有 false negative |
| "Compaction 可以删掉所有旧版本和 tombstone" | 有活跃 Snapshot 要保留；tombstone 只有落到**最底层**才能丢 |
| "Iterator 全程不物化" | **SSTable 懒**，但 MemTable 先 `Entries()` 物化成 vector（`db.cc:1859/1862`） |
| "internal key 就是 key + sequence" | 是 `user_key + '\0' + 8 字节 (max − sequence)`，**倒序**让新版本排前面（`format.cc:78`） |
| "写路径是并发的" | `write_mu_` **全串行**（`db.cc:437`），每条 batch 一次 fsync，无 group commit |
| "恢复靠扫目录猜" | 靠 **CURRENT → MANIFEST replay**；只有 Manifest 缺失才退化到扫目录（`db.cc:1248`） |

---

# 附：面试高频追问 TOP 8

这 8 个是 LSM 面试最容易被追问、也最容易答混的点，优先练：

1. **WAL 为什么必须先于 MemTable**（B1）
2. **sequence number 一个数字怎么管三件事**（D1）
3. **Get 为什么按 sequence 选而不是按文件顺序**（C5）
4. **Compaction 为什么不能随便删 tombstone**（G2）
5. **有 Snapshot 时 compaction 怎么保留版本**（G3）
6. **internal key 为什么倒序编码**（D3）
7. **崩溃恢复的完整顺序**（F2）
8. **已知缺陷：分层退化成全量重写**（G5）

**练法**：合上这份文档，用手机计时 90 秒，对着空气讲一遍。讲不顺就回来看，看完再讲。
**能连续讲顺 5 题，比读完 30 题有用。**
