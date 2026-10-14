# 向量索引架构重构方案

> 2026-10-09 模块边界更新：当前实现为纯 `kv_core` + 可选 `vector_db`/`hnsw`；扩展每次打开从 KV 重建，KV 不维护索引文件。本文中的分段、持久化及旧 DB 向量接口描述属于历史设计或后续提案，实际接口以 README 和分离验收记录为准。

状态：**设计提案，未实现**
日期：2026-09-28
本方案合并并取代早期仅解决重启问题的索引持久化设计。

> **评审提示（2026-09-28）**：本方案已由 `docs/VECTOR_INDEX_ARCHITECTURE_REVIEW.md` 评审。
> 骨架通过，阶段 0 实测判定 **GO**（P 路切分不伤召回，反而提升）。
> 评审发现的路由、版本裁决和 FLOP 论证问题已修订；处理细节见评审记录。

---

## 0. 摘要

1. 实测瓶颈**不是** HNSW 算法本身，而是：同一批向量被插入图**三次**，且每次插入都**串行排在单一独占锁后面**。
2. 上一版方案解决了前半（重复构图），没碰后半。它还让索引继续当 LSM 的附属物 —— 而这正是重复构图的根源。
3. 本方案把索引从 LSM 中彻底解耦，拆成 **P 个互相独立的段**，按 `hash(key) % P` 路由。每段拥有自己的文件、自己的锁、自己的生命周期。
4. 5 万条/s 的目标在当前召回设置和本机硬件上**不可达**，§1.3 给出算式。本方案给出一个可辩护的替代目标。

---

## 1. 到底什么慢

### 1.1 实测事实

下表每个数字都是实测。条件一并列出，因为没有一个可以外推。

| 编号 | 事实 | 数值 | 条件 | 来源 |
| --- | --- | ---: | --- | --- |
| F1 | 端到端写入吞吐 | **316.93 条/s** | 6000×768, M=16, ef_c=200, batch=256, NEON, MemTable=1024, L0=4, L1=64 MiB, `--require-maintenance` | `docs/REVIEW_FIXES_20260916.md:74` |
| F2 | 其中 Close/drain 占比 | **11.69 s / 18.93 s (62%)** | 同一轮；6 次 flush + 1 次 compaction → 2 个 SSTable | 同上 |
| F3 | 纯 HNSW 插入（单线程） | **962.5 inserts/s**（3 轮中位数） | 5k×768, M=16, ef_c=200, NEON | `docs/REVIEW_FIXES_20260916.md:65` |
| F4 | 纯 HNSW 插入（单线程） | 379.0 inserts/s（3 轮中位数） | 同上，scalar | 同上 |
| F5 | 索引字节 / SSTable 字节 | **1.04×** | 65,695,164 vs 62,963,946 | [历史存储基线](REVIEW_FIXES_20260916.md#历史存储基线)（旧基线） |
| F6 | 召回率 | ef = 128 / 256 / 512 时为 0.762 / 0.876 / 0.941 | 20k×768, scalar, M=16, Gaussian | `docs/REVIEW_FIXES_20260916.md:48` |
| F7 | 召回率 | ef = 128 / 256 / 512 时为 0.874 / 0.956 / 0.992 | 20k×768, NEON, M=32, Gaussian | `docs/REVIEW_FIXES_20260916.md:50` |

另有一次早期实验（记录已随 `docs/zh/` 一并移除，不在树里）：把 flush 子索引改成「从共享图切片复制」后，写入 288.5 → **483.0** 条/s，Close 13.26 → **5.13** s。该实验只改了 flush 侧，compaction 仍在全量重建。**把它当作下界，不是预测值。**

### 1.2 时间花在哪

由 F1 与 F3，折算到每条向量：

```
端到端（含维护路径）: 18.93 s / 6000 = 3.16 ms
纯图插入            : 1.04 ms       (F3)
差值                : 2.12 ms       (67%)
```

也就是说，**约三分之二的墙上时间根本不是图插入** —— 是重复构图、flush/compaction 记账、以及 drain。把这块修掉，只能回到 F3 的 962.5 条/s，再往上没有。

### 1.3 为什么 5 万条/s 按当前设定不可达

**先说结论：量级结论成立，但下面两个输入都不是实测值，不能按实测语气写。**
（本节已按评审 V3 重写。）

单次插入的代价。**距离计算次数已实测**（编译期开关 `KV_HNSW_DISTANCE_COUNTER`，
在 `HNSWIndex::Distance` 里计数；探针 `hnsw_partition_probe distance`）：

```
实测（n=20000, dim=768, M=16, ef_c=200, i.i.d. Gaussian, NEON）：
    稳态距离计算次数/插入 = 9,810.6    （后半段均值；三次运行逐位相同）
    p50 = 9,766   p90 = 11,365   max = 16,045
    含早期廉价插入的全程均值 = 8,529.2

模型值：ef_c × M = 200 × 16 = 3,200          ← 模型低估 3.07 倍
单次 L2 的 FLOP ≈ 768 次乘 + 768 次加 ≈ 1,536 FLOP
单次插入 FLOP = 9,810.6 × 1,536 ≈ 15.07 MFLOP
```

要达到 5 万条/s：

```
50,000 × 15.07 MFLOP = 753.5 GFLOP/s
```

**分母用实测，不用反推。** 同一内核（768 维、L2、NEON、f64 `vmulq_f64/vaddq_f64` 非 FMA）
在容器内 linux/arm64 实测，三种访存模式各 5 轮取中位数：

| 访存模式 | 说明 | 中位数 GFLOP/s |
| --- | --- | ---: |
| `hot` | 热查询 + 64 向量工作集（全在缓存），**纯计算上限** | **9.16** |
| `stream` | 两个操作数都顺序推进 | 8.08 |
| `hnsw` | 热查询 + 20k 数组中随机目标 | **3.54** |

参数：`dim=768`、`iterations=200000`，FLOP 口径 = `2 × dim`。
`hnsw` 模式抖动达 2×（1.98–4.05），这些数字只用于定论量级。

**真实插入循环内达到的速率**（用上面的实测计数换算）：
`20,000 条 / ~48.6 s`，总计 1.706 亿次距离调用 → **5.36–5.67 GFLOP/s**。
落在 `hnsw`（3.54）与 `hot`（9.16）之间，符合预期 —— 图有局部性，
比均匀随机访问快，但够不到全热。

**该内核的实测计算上限是 ~9.2 GFLOP/s。** 旧稿写「4.7 GFLOP/s 约为本核 NEON 峰值的 10%」，
隐含峰值 ~47 GFLOP/s —— 对 f64 非 FMA NEON 不成立，该说法已删除。
旧稿的「4.7 GFLOP/s」也是用 3,200 这个模型值反推的，不是实测，现已撤回。

推论（全部基于实测）：

- 5 万条/s 需要 753.5 GFLOP/s。按真实插入循环达到的 5.4 GFLOP/s 算，需要约 **140 个核**；
  即使按内核理论上限 9.16 算，也需要约 **82 个核**。
- 8 核在 20k 规模上约 `8 × 411 ≈ 3,290 条/s`（单线程实测中位数 411.4 条/s × 8，理想线性）。
- 单机要摸到 5 万，只能把 `ef_c` 砍掉约 3 倍（200 → 64），代价是召回 —— 见 F6。

**结论比旧稿更强：不是 52 个核，是 82–140 个核。** 旧稿算出 52 核是因为它的分子
（3,200 次距离）和分母（4.7 GFLOP/s）被同一个模型同向压缩，比值侥幸落在同一量级。
现在两个量都是实测的，误差方向不再互相抵消。

**测量口径的注意事项：** 距离计数是确定性的（三次运行逐位相同），可跨机器比较；
GFLOP/s 速率和插入吞吐**不可**跨机器比较 —— 本次全部在容器（linux/arm64 VM）内测得，
该 VM 的绝对性能抖动可达 2×（同一二进制同参数连跑三次：336.1 / 419.6 / 341.0 条/s）。
容器数字只用于定论量级，不是验收值。

**因此这个目标必须重新表述。** 本重构可辩护的目标是：

> 在本机、开启维护路径的前提下，**保持召回不变**，达到 **3,000–5,000 条/s**。

按上面的 8 核实测（~3,290 条/s，纯图插入、无维护、无串行争抢），这个区间**贴在下沿**：
它把全部预算都给了图插入，没有给 §4 的串行段（评审 V6）和 flush/compaction 留余量。
实际能到多少，取决于 V6 的测量结果，不能默认。

---

## 2. 设计原则

| 编号 | 原则 | 理由 |
| --- | --- | --- |
| P1 | 索引是一等存储，不是 LSM 的附属物 | LSM 每次重构结构，现在都会拖着索引一起动。这个耦合是重复构图的根因。 |
| P2 | 一个写者、一张图、一把锁 | HNSW 插入会改写邻居表；多个写者共享一张图就必然被迫用全局锁。分段消除了共享。 |
| P3 | 每一层都能独立测试 | 现在的 `HNSWIndex` 有 1,056 行，同时干图算法、MVCC、并发控制和文件 I/O。 |
| P4 | 重启只读，绝不重建 | 重建是正确性兜底，不是常规路径。 |
| P5 | 磁盘上不存第二份向量 | 向量已经在 `.sst` 里。索引只存拓扑和引用。 |

---

## 3. 架构

### 3.1 三层

```
┌──────────────────────────────────────────────────────────┐
│  HNSWIndex            段集合、路由、合并                    │
│                       并行查询 fan-out + 归并               │
│                       不知道文件格式                        │
├──────────────────────────────────────────────────────────┤
│  HNSWSegment          一张图 + 它的文件                     │
│                       Save / Load / 追加增量                │
│                       不知道其他段                          │
├──────────────────────────────────────────────────────────┤
│  HNSWGraph            节点、向量池、邻接表                   │
│                       Insert / Search / SelectNeighbors     │
│                       不知道 sequence、磁盘、锁             │
└──────────────────────────────────────────────────────────┘
```

| 层 | 负责 | 必须不知道 |
| --- | --- | --- |
| `HNSWGraph` | `nodes_`、`vector_data_`、邻接表、层数抽样、贪心下降、beam search | `SequenceNumber`、tombstone、文件、mutex、其他段 |
| `HNSWSegment` | 一张 `HNSWGraph` + 它的版本表 + 它的文件 + 它自己的锁 | 其他段、合并策略 |
| `HNSWIndex` | 段数组、写线程→段的分配、查询 fan-out、结果归并、段合并 | 节点内存布局、距离内核、文件字节布局 |

这直接兑现 P3。`HNSWGraph` 可以在零 I/O、零线程的条件下测试 —— 现有的 `tools/hnsw_recall_probe.cpp` 驱动的正是这个面，可以继续用。

**MVCC 放哪。** 版本表（`key → map<sequence, VersionState>`）下沉到 `HNSWSegment`。这是可行的，
前提是路由必须是 `hash(key) % P`（§4）：同一个 key 的所有版本落在同一段，该段的版本表对这个 key
就是完备的，段可以独立回答「这个节点在 sequence S 下可见吗」，**跨段不需要任何版本裁决**。
若路由按写线程分配，段内版本表就不完备，跨段裁决无法正确实现 —— 见 §5 与评审 V2。

### 3.4 阶段 1 的实现（已完成，2026-09-28）

`HNSWGraph` 已落地：`include/hnsw_graph.h` + `src/hnsw_graph.cc`。
它不含 `SequenceNumber`、不含 tombstone、不含文件 I/O、不含锁 —— 三者已逐项核对。

**可见性怎么跨层传递。** 搜索时图需要知道「某个节点能不能进结果集」，但图不许知道 sequence。
解法是 `HNSWNodeVisibility` 抽象接口：`HNSWIndex` 在每次查询的栈上构造一个
`SequenceVisibility(this, read_sequence)` 传进去，因此**并发查询之间不共享任何可变状态**，
`read_sequence` 仍然随调用传递，语义与重构前逐字一致。遍历不受影响：不可见节点照样
参与导航，只是不进结果集 —— 这正是重构前 `SearchLayer` 的行为。

**文件格式没变。** 图的节点不再存 sequence，`Save` 从版本表反推 `node_id → sequence`
（并断言每个节点都有唯一版本）。等价性验证见 §3.5。

### 3.5 阶段 1 的等价性验证

纯重构必须证明「行为不变」，测试全绿只是必要条件。三项确定性检查：

| 检查 | 方法 | 结果 |
| --- | --- | --- |
| 图遍历 | `KV_HNSW_DISTANCE_COUNTER=ON`，20k×768、M=16、ef_c=200 | 总距离调用 **170,584,778**，与重构前逐位相同；p50/p90/max/min 全部相同 |
| 持久化 | 同一 `vector_bench` 配置写出的 `.hnsw` 文件 | SHA-256 **字节相同**（`7510ff0d…`、`6ddc4619…`） |
| 召回 | `hnsw_recall_probe recall 5000 128 16 200 60` | 18 行召回曲线 **逐行相同** |

对照方式：用 `git worktree` 检出 HEAD（`7264f91`）单独构建一份，跑同一命令对比。
测试套件在 Release、ASan+UBSan、TSan 三种构建下均 6/6 通过，零编译警告。

### 3.2 段模型

```
                      ┌────────────────────────────────┐
   查询 ─────────────▶│  HNSWIndex::Search             │
                      │  fan-out，然后归并 top_k        │
                      └───┬────────┬────────┬──────────┘
                          │        │        │
                     ┌────▼──┐ ┌───▼───┐ ┌──▼─────┐
                     │ 段 0  │ │ 段 1  │ │ 段 P-1 │
                     │ 独立锁│ │ 独立锁│ │ 独立锁 │
                     └───┬───┘ └───┬───┘ └──┬─────┘
                         │         │        │
   写线程 0 ─────────────┘         │        │
   写线程 1 ───────────────────────┘        │
   写线程 P-1 ──────────────────────────────┘
                         │         │        │
                    seg0.hseg  seg1.hseg  ...   （每段一个文件）
```

**段路由必须是 key 的确定性纯函数：`segment = hash(key) % P`。** 这是硬约束，不是实现选择 ——
§7 的单段重建、§10 的迁移、R5 的覆盖集校验都要求能按段枚举 SSTable 条目，只有 key 的纯函数能满足。
（早期草案把段绑到写线程的 thread-local 上，热路径更便宜，但它不是 key 的函数：
重启后无法复现，上述三条全部落空。见评审 V1。）

**P 在启动时固定，取配置写线程数的 4 倍（默认 32）。** 段的数量**不增长**，
因此没有 LSM 式合并阶梯要管，查询 fan-out 也有上界。

P 取 4 倍而不是等于写线程数，是因为 hash 路由下写线程按 key 落段，这是个球箱问题：
P = T = 8 时期望只有 `8 × (1 − (7/8)^8) = 5.25` 个段被同时占用，白白损失约 34% 的并行度；
P = 32、T = 8 时期望占用 7.17 段（89.6%）。代价是查询 fan-out 要搜更多段 —— 见 §3.3。
这个权衡在阶段 3 用 P ∈ {8, 16, 32} 实测写吞吐与查询延迟后再定稿。

### 3.3 为什么是分段而不是单图

单张全局图必然导致单把独占锁（`src/hnsw_index.cc:157`，`mutable std::shared_mutex mutex_`），因为 HNSW 插入会改写它并不拥有的节点的邻居表。拆成 P 张图就把这把锁拆开了：一次插入只碰 `hash(key) % P` 那一个段，其余 P−1 个段完全不被触及。

有两个代价必须明说。**阶段 0 已经把这两个都测过了（结果见 §11.1），结论与本文档原稿相反。**

1. **查询 fan-out。** 一次查询要搜全部 P 个段，总搜索量确实变成约 P 倍。但「单段变小」省下来的
   比原稿估计的多：实测顺序 fan-out 的 QPS 损失是 **1.8–14×**，不是 P 倍。
   而且各段等大，所以并行 fan-out 的墙上时间 ≈ 单段耗时，并行 QPS ≈ P × 顺序 QPS。
   在 iso-recall 口径下，P = 8 在三种分布上推算加速 2.4× / 2.8× / 7.0×。
   **fan-out 仍然必须并行，不能顺序** —— 这一点原稿说对了。
2. **召回率。** 原稿担心退化，实测是**显著提升**：20k×768、M=16、P=8、每段 ef=128 时，
   i.i.d. Gaussian 0.7440 → 0.9840，独立中心 OOD 0.5670 → 0.9767，而单图在 ef=512
   都够不到 0.9767。**召回上限被抬高了**，不是预算堆出来的。

退路（保留单图、改为批次层面并行）仍然保留，但阶段 0 之后不再需要它作为主路径。

---

## 4. 写入路径

```
DB::PutVector(key, vector, metadata)
  ├─ WAL 追加                                    （不变）
  ├─ MemTable::PutVector                         （不变）
  └─ HNSWIndex::Insert(key, seq, vector, meta)
       └─ segments[hash(key) % P]->Insert(...)   ← 一把锁、一张图
```

`hash(key) % P` 是 key 的纯函数，热路径上不需要任何 atomic、缓存或线程状态。
附带性质：**同一 key 的所有版本永远落在同一段** —— 这正是 §5 需要的。

与现状的差异：

| 现在 | 改后 |
| --- | --- |
| `HNSWIndex::InsertVersion` 取全局 `unique_lock` | 每段取自己的锁；key 落在不同段的写者互不争抢，落在同段的才排队 |
| flush 调 `SaveVectorIndexEntries` → 重建子索引 | flush 调 `HNSWIndex::FlushDeltas()` → 每段往自己的文件**追加增量** |
| compaction 调 `BuildVectorIndexForCompaction` → 全量重建 | compaction 调 `HNSWIndex::ApplyDroppedVersions()` → 对被淘汰版本逐个 `MarkDeleted`，每个 O(1) |

compaction 这一改动与上一版方案是同一个洞察，也仍然是单笔价值最高的一项：`MarkDeleted` 只动版本表，从不碰 `nodes_` 或任何邻居表，所以不扰动图结构。

**顺序约束（沿用上一版，仍然必须遵守）：** 版本对齐必须在 compaction **发布之后**、在同一个 `version_mu_` 临界区内执行。`MarkDeleted` 不可回滚，而 compaction 可能在发布前失败。

**串行段已测量（评审 V6，2026-09-28）。** 图插入移出临界区后，剩下的串行段是 WAL 追加 + MemTable 更新 + flush 记账。在容器内用 `vector_bench` 隔离测量（6000 × 768、batch=256、MemTable=1024、L0=4、`--require-maintenance`，各 3 轮取中位数）：

| 配置 | 吞吐 | 每条耗时 | Close | flush / compaction |
| --- | ---: | ---: | ---: | --- |
| A 完整（M=16, ef_c=200） | 267.0 条/s | 3,745 µs | 14.0 s | 6 / 1 |
| B 图插入近似免费（M=2, ef_c=1） | **5,663.4 条/s** | **177 µs** | 0.75 s | 6 / 1 |

B 把图插入压到接近零，剩下的就是串行段 + 维护。**它的上限是 ~5,600 条/s，只占 A 的墙上时间的 4.7%。**

结论：**串行段不是瓶颈，§11 的 3,000–5,000 条/s 目标不被它封顶**（上沿 5,000 仍在 5,600 之下）。
A 的 95% 时间花在图插入（含重复构图）上 —— 这正是本重构要消灭的部分。

注意 A 复现了 F1/F2 的量级与比例：F1 是 316.93 条/s、Close 占 62%；这里 267.0 条/s、Close 占 62%（14.0 / 22.6）。
容器绝对性能略低于 macOS 主机，比例一致。

---

## 5. 读取路径

```
HNSWIndex::Search(query, top_k, ef_search, read_sequence)
  ├─ fan-out：对每个段 s，并行执行
  │      s->Search(query, top_k, ef_search, read_sequence)
  │        → 段内 top_k，用该段自己的版本表过滤
  ├─ 归并：合并所有段内结果，取全局 top_k
  └─ 返回
```

**版本裁决完全在段内完成，归并层不参与。** 因为路由是 `hash(key) % P`（§4），
一个 key 的所有版本必然落在同一个段里，所以段自己的版本表对该 key 是**完备**的 ——
它可以独立回答「这个节点在 sequence S 下可见吗」，不需要问别的段。

于是 `HNSWSegment::Search` 的过滤逻辑与今天单图的 `IsVisible`
（`src/hnsw_index.cc:264-281`）**完全一致，可以直接复用已经验证过的代码**：

- 段内可能有同一 key 的多个节点（不同版本）。取 `sequence ≤ read_sequence` 的最大版本；
- 若它是 tombstone，或它的 `node_id` 不是当前节点，则该节点不可见；
- 因此「段 0 有 live、段 1 有 tombstone」这类跨段复活问题**在 hash 路由下不存在** ——
  两个版本本就在同一段，段内版本表直接给出正确结论。

归并层只需要按距离做一次全局 top-k。按 key 去重仍然要写，但它是**防御性**的
（hash 路由保证不会出现重复 key），不是正确性的依赖。

三个容易写错的地方（已按上面的结论重写）：

1. **归并不能用「按 key 取各段最大 sequence」来补版本裁决。** 段内搜索按距离截断 top_k，
   归并层只能看到各段返回的候选；如果某个 key 的最新版本（尤其 tombstone）没落在任何段的
   局部 top_k 内，归并就无从知道它存在。**任何把版本裁决放在归并层的方案都不成立**
   —— 这也是本方案坚持 key 的确定性路由的原因（评审 V2）。
2. **`entry_point_` 现在是每段一个。** 不再有全局入口点，每段各自开始下降。
3. **`read_sequence` 必须一路传到段内**，不能只在归并时用。

并行 fan-out 用 `HNSWIndex` 自持的**固定大小线程池**，不要每次查询 `std::async` —— P = 32 时线程创建开销会盖过收益。线程池大小与 P 的关系（一个查询线程负责几个段）需要实测。

---

## 6. 持久化

### 6.1 文件布局

```
<db>/
  MANIFEST-<n>
  <n>.sst
  hnsw/
    seg-0.hseg        段 0：完整快照
    seg-0.delta       段 0：快照之后追加的增量
    seg-1.hseg
    seg-1.delta
    ...
```

每段采用与 LSM 本身相同的思路：**基础快照 + 增量日志**。增量日志只存拓扑变更 —— 向量只被引用，绝不复制（P5）。

```
增量记录 := { sequence: u64, node_id: u64, op: u8, level: u8, neighbour_ids: u64[] }
             op = INSERT  每层一条，neighbour_ids 该层的邻居
             op = DELETE  level 与 neighbour_ids 留空（tombstone）
```

不含向量。重放时，一条 INSERT 从拥有该 sequence 的 SSTable 里取回向量。这让增量很小（每条几十字节），并消除了磁盘上第二份向量副本 —— 也就是今天索引文件达到 SSTable 1.04 倍（F5）的原因。

**两个必须写进格式的规则（评审 V5）：**

1. **DELETE 必须进增量。** tombstone 也要落盘。否则「快照之后、下次快照之前被删除的 key」
   在重启重放时没有记录，会被旧快照里的活版本复活。
2. **重放时若向量已不可解析，必须连同该 node_id 的全部边一起丢弃。**
   compaction 会淘汰被 tombstone 覆盖的旧版本，此时该 sequence 的向量**已经不在任何 SSTable 里**，
   「从 SSTable 取回向量」这条路径会失败。只跳过向量是不够的 —— 图里会留下一个「有边、无向量」
   的节点，邻居指向空洞。正确做法是丢弃该 node_id 的**全部**层上的邻居边（即整条 INSERT 记录），
   并把它当作从未插入过。

### 6.2 快照与增量轮转

```
Close()            ：对所有段做快照，然后截断各自的增量
增量 > 阈值         ：只对该段在后台做快照，截断其增量
```

单段快照是 O(该段节点数) = O(N/P)，且发生在写路径之外。后台快照只需单段的一致视图，持锁时间远小于整索引快照。

### 6.3 打开路径

```
DB::Open()
  ├─ LoadOrRecoverManifest()
  ├─ 对每个存在的 seg-i.hseg：
  │      Load 基础快照
  │      重放 seg-i.delta（INSERT 从 SSTable 解析向量；解析不到的整条丢弃）
  ├─ Recover()   → WAL 重放；按 hash(key) % P 路由到各段
  └─ 就绪
```

没有 `MergeFrom`，也没有全量重建。段文件缺失或损坏时，只重建**那一段**，来源是满足 `hash(key) % P == i` 的 SSTable 条目。

**顺序约束：** 段文件必须在 `Recover()` 之前加载完，因为 WAL 重放会发起常规插入，需要有活着的段可路由。

---

## 7. 段生命周期

```
  ACTIVE ──── 增长，接受写入 ────────────────────────────────┐
     │                                                       │
     │ （仅当某段因重平衡被退役时）                            │
     ▼                                                       │
  SEALED ── 只读，仍被查询 ──▶ 快照 ──▶ DELETED              │
                                                             │
  段身份不变；P 固定 ◀───────────────────────────────────────┘
```

因为 P 固定，没有合并阶梯。段不绑定线程（路由是 key 的函数，不是线程的函数），所以
「某个写线程死掉导致它的段闲置」这种情形不存在 —— 只有运维显式重平衡（例如在线改 P）
才封存段。这条路径不在阶段 1 范围内，列在 §15。

**tombstone 累积**在段内依然存在。上一版的阈值规则照搬过来，现在按段执行：当某段 `node_count > 2 × live_node_count` 时，用该段对应的 SSTable 条目重建**这一段**。因为是单段，重建是 O(N/P)，且可以在其他段继续服务时进行。

**「该段对应的 SSTable 条目」现在有明确定义**：满足 `hash(key) % P == i` 的那些条目。
这正是 §4 要求路由必须是 key 的纯函数的原因 —— 若路由按写线程分配，这个集合无法从
SSTable 内容里枚举出来，单段重建就无法实现（评审 V1）。

---

## 8. 并发模型

| 操作 | 持有的锁 |
| --- | --- |
| 向段 *i* 插入 | `segments[i]->mutex_`（独占），*i* = `hash(key) % P` |
| 查询段 *i* | `segments[i]->mutex_`（共享） |
| 快照段 *i* | `segments[i]->mutex_`（共享） |
| 版本对齐（compaction 后） | `DB` 的 `version_mu_`，然后短暂取各段独占锁 |
| `sstables_` 交换 | `version_mu_`（不变） |

关键变化：**`write_mu_` 不再需要串行化图插入**，因为一次插入只触及一个段。它仍需串行化 WAL 追加和 MemTable 修改 —— **这一段的开销尚未测量，见 §4 末尾与评审 V6。**

hash 路由下的锁争抢是可算的：P 个段、T 个写线程，期望被同时占用的段数是
`P × (1 − (1 − 1/P)^T)`。P = T = 8 时只有 5.25，P = 32、T = 8 时是 7.17 —— 这是 §3.2 取 P = 4T 的依据。

吞吐增益来自把全局锁从图变更路径上拿掉，而不是来自段变小。段变小带来的是**召回上限的提升**（阶段 0 实测，见 §11），那是另一份收益。

---

## 9. 接口草图

```cpp
// hnsw_graph.h —— 纯结构：无 I/O、无锁、无 sequence
class HNSWGraph {
 public:
  explicit HNSWGraph(HNSWGraphOptions options);

  NodeId AddNode(std::vector<float> vector, std::string key,
                 std::string metadata);              // 没有 sequence 参数
  void ConnectTo(NodeId node, const std::vector<NodeId>& neighbours, int level);

  std::vector<Candidate> Search(const float* query, size_t ef, int level) const;
  std::vector<Candidate> SearchLayer(const float* query,
                                     const std::vector<NodeId>& entry_points,
                                     size_t ef, int level) const;

  void Clear();
  size_t Size() const;
  GraphStats Stats() const;
 private:
  // nodes_, vector_data_, entry_point_, max_level_, level_multiplier_，
  // 以及 thread_local 访问标记机制（仍按线程、仍用 generation）
};

// hnsw_segment.h —— 一张图、一个文件、一把锁
class HNSWSegment {
 public:
  Status Insert(std::string key, SequenceNumber seq,
                std::vector<float> vector, std::string metadata);
  Status MarkDeleted(std::string key, SequenceNumber seq);

  Status Search(const std::vector<float>& query, size_t top_k, size_t ef,
                SequenceNumber read_sequence,
                std::vector<VectorResult>* out) const;

  Status SaveSnapshot(const std::filesystem::path& path) const;
  Status LoadSnapshot(const std::filesystem::path& path);
  Status AppendDelta(const std::filesystem::path& path);
  Status ReplayDelta(const std::filesystem::path& path, const VectorResolver&);

  SegmentStats Stats() const;
 private:
  HNSWGraph graph_;
  std::unordered_map<std::string, std::map<SequenceNumber, VersionState>> versions_;
  std::uint64_t next_node_id_ = 0;
  mutable std::shared_mutex mutex_;
};

// hnsw_index.h —— 路由、fan-out、归并
class HNSWIndex {
 public:
  Status Open(const std::filesystem::path& dir, size_t segment_count);
  Status Insert(std::string key, SequenceNumber seq,
                std::vector<float> vector, std::string metadata);
  // 必须存在：MemTable::Put / Delete（src/memtable.cc:15、:45）依赖它。
  // 与 Insert 走同一条路由，否则同一 key 的 tombstone 会落到别的段。
  Status MarkDeleted(std::string key, SequenceNumber seq);
  Status Search(const std::vector<float>& query, size_t top_k, size_t ef,
                SequenceNumber read_sequence,
                std::vector<VectorResult>* out) const;
  Status ApplyDroppedVersions(const std::vector<DroppedVersion>&);
  Status FlushDeltas();
  Status SnapshotAll();
  IndexStats Stats() const;
 private:
  std::vector<std::unique_ptr<HNSWSegment>> segments_;
  std::unique_ptr<QueryPool> query_pool_;      // 固定大小，用于 fan-out
  // key 的确定性纯函数，无状态、无 atomic。§7 的单段重建、§10 的迁移、
  // R5 的覆盖集校验都依赖它是纯函数。
  size_t SegmentOf(std::string_view key) const;
};
```

注意移动了什么：`SequenceNumber` 在 `HNSWGraph` 里**完全消失**。仅这一项就干掉了目前贯穿 `SearchLayer` 的 `read_sequence == nullptr` 分支（`src/hnsw_index.cc:184`、`:218`、`:244`），并让图可以独立测试。

---

## 10. 迁移

| 步骤 | 动作 |
| --- | --- |
| 1 | 首次打开旧库时（每 SSTable 一个 `<n>.hnsw`、没有 `hnsw/` 目录），从 SSTable 条目重建所有段，按 `hash(key) % P` 分配 |
| 2 | 写出 `hnsw/seg-*.hseg`；此后打开走读取路径 |
| 3 | 旧的 `<n>.hnsw` 文件原地保留；删除是破坏性操作，必须由运维发起 |
| 4 | `SSTableMeta::index_path` 保留但标记废弃，使旧库仍可打开 |

迁移是一次性 O(N) 重建，正好是今天的常规路径 —— 已被充分验证过的代码。

---

## 11. 预期收益

| 项 | 依据 | 预期 |
| --- | --- | --- |
| 消除重复构图 | F2（62% 墙上时间不是插入）+ 早期切片实验（288.5 → 483.0 条/s） | **实测下界 1.67×。** 若能摸到 F3 的 962.5 条/s 则是 3.0× |
| 并行插入 | F3 × P，忽略协调开销 | P = 8 时上限约 7,700 条/s。**串行段上限已实测为 ~5,600 条/s（§4），目标区间 3,000–5,000 在它之下，不被封顶** |
| 重启 | 今天：O((N − \|file1\|) × ef_c × dim) 重建 | 纯读取加载。N = 20k、dim = 768 时：**段文件只存拓扑，约 3.8 MB**；向量（61.4 MB）按 §6.1 从 SSTable 读回，不在段文件里。合计读 ~65 MB，顺序读 + 反序列化**估算** 0.2–0.5 s。这是由字节数推出的估算，**不是实测** |
| 磁盘 | F5：索引 = SSTable 的 1.04× | 向量不再在磁盘上重复，索引退化为纯拓扑 |
| 召回 | 阶段 0 实测（见 §11.1） | **不退化，且在 OOD 上显著提升。** 原稿这一行写的是「未知」，现已实测 |

### 11.1 阶段 0 实测（2026-09-28）

探针 `tools/hnsw_partition_probe.cpp`，容器内 linux/arm64 + NEON。
20,000 × 768、300 查询、ef_c = 200、随机 1/P 分片（同时建模 thread-local 与 hash 两种路由，
两者都与语料几何无关）。M = 16、P = 8（每段 2,500 节点）：

| 查询分布 | 单图 ef=128 | 单图 ef=256 | 单图 ef=512 | P=8 每段 ef=64 | P=8 每段 ef=128 |
| --- | ---: | ---: | ---: | ---: | ---: |
| i.i.d. Gaussian | 0.7440 | 0.8697 | 0.9373 | 0.9357 | **0.9840** |
| 共享中心聚类 | 1.0000 | 1.0000 | 1.0000 | 0.9987 | 1.0000 |
| 独立中心聚类（OOD） | 0.5670 | 0.7310 | 0.8923 | 0.9087 | **0.9767** |

M = 32 同向：i.i.d. 单图 ef=512 = 0.9883，P=8 每段 ef=128 = 0.9987；
OOD 单图 ef=512 = 0.8977，P=8 每段 ef=128 = 0.9870。

**召回上限也被抬高**：OOD 上单图在 ef=512 只有 0.8923，P=8 在每段 ef=128 就到 0.9767 ——
单图在测试范围内够不到这个水平。

**代价。** 顺序 fan-out 的 QPS 损失 1.8–14×。各段等大 ⇒ 并行 QPS ≈ P × 顺序 QPS。
iso-recall 口径下的**推算**加速：i.i.d. ~7.0×、共享中心聚类 ~2.8×、OOD ~2.4×。
**并行列是推算值，阶段 4 必须实测**（探针没有线程池）。

原始输出 `bench_runs/stage0-partition/*.txt`；完整评审见 `docs/VECTOR_INDEX_ARCHITECTURE_REVIEW.md`。

---

## 12. 风险

| 编号 | 风险 | 严重度 | 缓解 |
| --- | --- | --- | --- |
| R1 | 归并后的召回比单图退化 | ~~高~~ **已排除** | 阶段 0 实测不退化、反而提升（§11.1） |
| R2 | 查询延迟随 P 增长 | 中 | 固定线程池并行 fan-out；在 P = 1, 2, 4, 8, 16, 32 下测 p50/p99 |
| R3 | 跨段版本裁决写错（tombstone 复活） | ~~高~~ **已消解** | 路由改 `hash(key) % P` 后同 key 版本同段，段内版本表完备，跨段不参与裁决（§5、评审 V1/V2）。仍要测「段内先 live 后 delete」 |
| R4 | 增量重放比预期慢 | 中 | 用快照阈值把增量压小；在阈值处实测重放耗时。另见 V5 的两条格式规则 |
| R5 | 段文件与 SSTable 集合不一致 | 高 | 每份快照头里存 `covered_sequence` + `covered_files`；不匹配则只重建该段。**前提是段路由是 key 的纯函数**，否则无法枚举该段的条目（评审 V1） |
| R6 | 内存：P 张图带来 P 份每图开销 | 低–中 | 该开销按段固定、不随节点数增长；在 P = 32 下实测。另注意 fan-out 时每查询线程每段一份访问标记，总量 O(查询线程数 × N) |
| R7 | 后台快照与写入竞争 | 中 | 快照持该段的共享锁；测试计划含 ASan + TSan 覆盖 |
| R8 | 串行段（WAL + MemTable）封顶并行收益 | ~~高~~ **已排除** | 实测上限 ~5,600 条/s，只占当前墙上时间的 4.7%（§4、评审 V6）。目标区间在其之下 |

---

## 13. 分阶段实施

每个阶段结束时树都是可用的，且可独立度量。

| 阶段 | 改动 | 交付物 | 风险 |
| --- | --- | --- | --- |
| **0** | **在写任何代码之前，先测 P 路切分的召回。** 已执行，判定 **GO**，结果见 §11.1 | 整个设计走 / 不走的决定 | 无（已完成） |
| 1 | 从 `HNSWIndex` 中抽出 `HNSWGraph`，行为不变 | **已完成（2026-09-28）**，见 §3.4 | 低 |
| 2 | 抽出 `HNSWSegment`（图 + 版本 + 文件），仍是单段 | 行为不变，边界更新 | 低 |
| 3 | 引入 P 个段 + `hash(key) % P` 路由 + 并行插入；**同时实测 P ∈ {8, 16, 32} 的写吞吐与 §4 串行段上限** | 吞吐提升，P 定稿 | 中 |
| 4 | 并行查询 fan-out + 归并 | 延迟持平 | 中 |
| 5 | 快照 + 增量持久化；删除 `SaveVectorIndexEntries` 与 `BuildVectorIndexForCompaction` | 重启变为纯读取 | 高 |
| 6 | 按段的 tombstone 重建 | 内存有界 | 低 |
| 7 | 迁移 + 旧文件清理工具 | 可运维 | 低 |

阶段 0 刻意排在最前。它便宜、是一次性的，而且它决定阶段 3–4 到底值不值得建。**现已完成。**

---

## 14. 测试计划

| 领域 | 测试 |
| --- | --- |
| **召回（门禁）** | P 路切分 vs 单图：20k×768，M = 16/32，单图 ef ∈ {128, 256, 512}、每段 ef ∈ {16…512}，Gaussian + 聚类 + OOD 三种分布。**查询数 ≥ 300**：100 查询 × Recall@10 的 SE = 0.69 个点，原「1 个点」的门限只有 1.45 SE，会被噪声击穿（评审 V4）。门限改为「**超出运行间噪声**」，而不是固定点数。复用 `tools/hnsw_partition_probe.cpp`（已完成阶段 0） |
| **召回（重构后）** | 每个阶段后跑同一矩阵；任何超出运行间噪声的下降都阻断该阶段 |
| 版本归并 | hash 路由下同一 key 的版本必然同段，**重点测「同一 key 在段内先 live 后 delete」**，以及归并层不会引入跨段版本裁决。见 §5 |
| 并发 | 压不同 key 空间制造段争抢；ASan + TSan 构建；P ∈ {8, 16, 32} 下测实际并行度与球箱模型的偏差（§8） |
| 持久化 | 快照 + 增量往返；截断的增量；损坏的 CRC；`kill -9` 后的增量重放；**增量中已被 compaction 淘汰的 INSERT 记录**（评审 V5） |
| 迁移 | 打开旧库；确认发生一次重建；确认下次打开走读取路径 |
| 端到端 | `vector_bench --mode write --require-maintenance` 前后对比，吞吐旁必须同时报 flush 和 compaction 次数（`docs/REVIEW_FIXES_20260916.md:15` 的要求）。**另需单独测 §4 的 WAL + MemTable 串行段上限**（评审 V6） |
| 查询延迟 | P = 1, 2, 4, 8, 16, 32 下的 p50/p99；验收条件是「不差于 P = 1，误差范围内」 |

现有 CI 门禁（`KV_ENABLE_RECALL_REGRESSION`）应扩展为同时跑两条打开路径和两个 P 值，而不是另起一套并行测试。注意现有 20k 回归的 timeout 已是 900 s，×4 后需要重新评估 CI 时间预算。

---

## 15. 未决问题

1. ~~**阶段 0 的结论。**~~ **已解决（2026-09-28 实测，判定 GO）。** P 路切分不伤召回，
   反而在三种分布上全部提升，OOD 上把单图够不到的召回上限抬高了。结果见 §11.1。
   原门限「ef = 512 掉超过 1 个点」也已作废 —— 那个门限比噪声还小（评审 V4），
   改用 300 查询 + 「超出运行间噪声」。
2. **P 的取值。** 改为 P = 4 × 写线程数（默认 32），依据是 hash 路由下的球箱模型（§3.2）。
   但 P 增大会抬高 fan-out 成本，需要在阶段 3 用 P ∈ {8, 16, 32} 实测写吞吐与查询延迟后定稿。
   是否需要运行时可调？在线改 P 会触发 §7 的段封存/重平衡，代价未评估。
3. **增量阈值。** 增量大到多少会让重启重放变慢？在实测出数字之前不设阈值。
4. **重平衡。** 段不绑定线程，所以「写线程死掉导致段闲置」不存在。剩下的场景只有在线改 P，
   需要把旧 P 下的段按新 P 重新切分 —— 代价是 O(N) 重建。不在阶段 1 范围内。
5. **`MergeFrom` 的去留。** 段路径落地后它就是死代码。删掉，还是留给旧库兜底路径？
6. **5 万这个目标。** §1.3 论证了它在当前召回设置下不可达。原本的意图是「任何召回下的 5 万条/s」，还是「F6/F7 召回下的 5 万条/s」？设计完全不同。
7. ~~**串行段的吞吐上限。**~~ **已解决（2026-09-28 实测）。** 上限 ~5,600 条/s，
   只占当前墙上时间的 4.7%，不封顶目标区间。见 §4。
