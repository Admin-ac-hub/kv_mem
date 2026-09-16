# 向量数据库改造 —— 状态评审

日期：2026-09-16
范围：`VECTOR_DB_ROADMAP.md` 的 Week 1–6 交付物
性质：内部评审记录，不是对外交付物

> 以下正文记录初次评审时的状态。2026-09-16 的修复、复测与剩余项目见
> [修复记录](REVIEW_FIXES_20260916.md)，不要把历史结论当作当前代码状态。
>
> 口径修正：反向边采用多样性启发式并不单独构成缺陷；原聚类探针的查询使用
> 独立中心，测的是分布外查询，不能据此排除数据分布影响。提高 ef_construction
> 的单组结果也不足以排除构建质量因素。1000 节点、M=16 的图并非接近全连接。
> 每 batch fsync 是否限制到 5 万/s 取决于 batch 大小与同步延迟，不能断言不可能。
> E1/E2 已由前序操作处理：当前没有暂存文件，`/build-*/` 已忽略构建目录。

---

## 1. 结论

Week 1–6 的**功能代码基本写完且可运行**（构建通过，`ctest` 5/5 全绿），但**不能算"做完"**：

1. 路线图「核心功能（必须实现）」里**混合检索整块缺失**；
2. 可观测性指标没有进入引擎；
3. ANN 的**性能目标和 Recall 目标都没达标**，而且 1M × 768 的正式验收从未执行；
4. Recall 偏低是 **HNSW 实现本身**的问题，不是数据或 LSM 集成的问题；
5. 仓库处于危险状态：170 个构建产物已进入暂存区，Week 1–6 的主要工作尚未提交。

---

## 2. 已验证可用的部分

| 项 | 证据 |
| --- | --- |
| 构建 | Debug 与 Release 均无错误完成 |
| 测试 | `ctest` 5/5：`kv_test`、`hnsw_test`、`vector_bench_smoke`、`kv_stress_fixed_seed`、`week5_bench_sync_smoke` |
| 向量编码 | `vector_value.cc` magic/element_type/dim/vector/metadata，编解码与维度校验齐全 |
| HNSW 索引 | 指数分层、贪心下降、层内 beam search、启发式邻居选择、双向连边 |
| 索引持久化 | `Save`/`Load`/`MergeFrom`，CRC32 校验，Manifest 记录索引路径 |
| Compaction | 两阶段发布 + 原子替换，构建期查询走旧索引 |
| SIMD | 运行时探测，arm64 回退标量路径 |
| 批量查询 | `SearchBatch` 单次持共享锁 |

---

## 3. 路线图未完成项

| 路线图条目 | 状态 | 证据 |
| --- | --- | --- |
| 4. 混合检索（标量预过滤 / 范围+向量 / 分数融合） | **未实现** | 全仓库 grep 不到 `metadata_filter`；`DB::Search` 无过滤参数 |
| 可观测性：查询指标（QPS/P50/P99/Recall） | **未进引擎** | 仅出现在 benchmark stdout |
| 可观测性：插入指标（吞吐/构建耗时） | **未进引擎** | 同上 |
| 可观测性：存储指标（SSTable/索引/内存占用） | **部分** | benchmark 侧统计文件字节数，引擎无接口 |
| 3. 性能目标（1M、P99 < 10ms、Recall@10 > 0.95） | **未验收** | `bench_runs/` 只有一次 100 × 8 的 smoke |

`DBStats` 覆盖的是 KV 侧指标（SSTable 数、cache hit/miss、bloom 过滤次数），
`HNSWStats` 覆盖图结构（节点数、边数、层分布），两者都不包含延迟分位数和 Recall。

---

## 4. 实测结果

环境：macOS arm64，Release（`build-week6-bench`），20000 × 768 Float32，
`M=16`、`ef_construction=200`、`ef_search=128`、`write_batch=256`。

| 指标 | 实测 | 路线图目标 | 差距 |
| --- | ---: | ---: | --- |
| 写入吞吐 | 498 /s | > 50000 /s | ~100× |
| 查询 QPS | 1208 | > 10000 | ~8× |
| 查询 P99 | 1.10 ms | < 10 ms | 达标 |
| Recall@10 | 0.5000 | > 0.95 | 不足一半 |
| SSTable 字节 | 62,963,946 | — | 约等于原始向量体积 |
| 索引字节 | 65,695,164 | < 向量数据 2 倍 | 1.04×，达标 |

### 4.1 Recall 不达标 —— 定位到 HNSW 图本身

用 `tools/hnsw_recall_probe.cpp` 直接驱动 `HNSWIndex`（无 LSM、无 MVCC 过滤、
结果无重复 key），排除集成层干扰：

| 配置 | ef=16 | ef=32 | ef=64 | ef=128 | ef=256 | ef=512 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 20000 × 768, M=16, i.i.d. Gaussian | 0.195 | 0.325 | 0.440 | 0.620 | 0.775 | 0.890 |
| 20000 × 768, M=16, clustered | 0.200 | 0.290 | 0.300 | 0.555 | 0.860 | — |
| 20000 × 128, M=16, i.i.d. Gaussian | 0.325 | 0.410 | 0.550 | 0.720 | 0.830 | 0.885 |
| 20000 × 768, **M=32**, i.i.d. Gaussian | 0.305 | 0.465 | 0.640 | 0.800 | 0.910 | 0.965 |
| 20000 × 768, M=16, **ef_c=500** | 0.210 | 0.305 | 0.475 | 0.605 | 0.780 | 0.905 |

三条推论：

1. **不是数据问题。** 换成聚类数据（更接近真实 embedding 分布）召回反而更低，
   所以不能用「高维随机数据本身难」解释。
2. **不是构建质量问题。** `ef_construction` 从 200 提到 500 几乎无变化
   （0.620 → 0.605），说明搜索期的图遍历才是瓶颈。
3. **底层度数上限是主因之一。** `HNSWStats` 显示
   `directed_edges=341492`、`max_degree=16`、平均出度 17.1 —— 底层度数被
   `max_neighbors` 卡死在 16。标准 HNSW 的 level 0 度数上限应为 **2M = 32**，
   上层才是 M。把 M 提到 32 后 ef=128 的召回从 0.620 升到 0.800，验证了这一点。
   但 M=32 时 ef=128 仍只有 0.800，所以**邻居选择 / 反向边剪枝还有第二个问题**：
   `PruneNeighbors` 在反向边上反复套用多样性启发式，倾向剪掉长程边，
   使图的可导航性随 N 增长而退化。

**测试门限太松，掩盖了这个问题。** `hnsw_test` 的召回门限是 1000 × 128 上
`Recall@10 > 0.90`；这个规模下图几乎接近全连接，门限能过，但 20k 就崩到 0.72。

### 4.2 写入吞吐低 —— 瓶颈是 HNSW 插入，不是 fsync

| 路径 | 20000 × 768 耗时 | 吞吐 |
| --- | ---: | ---: |
| 纯 `HNSWIndex::Insert`（无 LSM） | 37.87 s | 528 /s |
| 完整 `DB` 写路径（WAL + MemTable + Flush） | 40.11 s | 498 /s |

DB 层只增加约 5% 开销，**95% 的时间花在 HNSW 插入上**，单次插入 1894 μs。
降低 `ef_construction` 到 40 也只有 796 /s，量级不变。

根因是写路径全程串行：`write_mu_` + `version_mu_` + `memtable_mu_` +
`HNSWIndex` 独占锁，且没有延迟索引构建。README 的「当前边界」已如实承认。

### 4.3 同一批向量被插入 HNSW 图两次

- 写路径：`MemTable::PutVector` → 全局索引 `InsertVersion`
- Flush：`DB::SaveVectorIndexEntries` 为这个 SSTable **重新构建一个全新的子索引**
  （同样 `ef_construction=200`），同一批向量再插一遍
- Compaction：`DB::BuildVectorIndexForCompaction` 还会再插一遍
  （output + 所有非输入表）

一次 flush 的成本因此约为两倍向量数的插入量。

### 4.4 `level1_size_limit` 阈值算错

`src/db.cc` 中：

```cpp
const std::uint64_t level1_size_limit =
    static_cast<std::uint64_t>(options_.memtable_entries_limit) * 64;
```

默认 `memtable_entries_limit = 1024`，得到 **64 KB**。而 768 维下 1024 条向量的
SSTable 约 3 MB，阈值恒为真 → **每次后台唤醒都会触发一次 Compaction**，
每次 Compaction 全量重建索引。

当前 benchmark 没暴露这个问题，因为它把 memtable 设成了 `expected_vectors + 1`
（见 4.5），根本没触发 flush/compaction。在默认配置下这是灾难性的。

### 4.5 benchmark 绕过了 flush / compaction 路径

`bench/vector_bench.cpp`：

```cpp
options.memtable_entries_limit =
    expected_vectors == std::numeric_limits<size_t>::max()
        ? expected_vectors
        : expected_vectors + 1;
options.block_cache_capacity = 0;
```

后果：

- 20000 条全部落在一个 MemTable，最终只产生 **1 个 SSTable、0 次 Compaction**
  （`MANIFEST` 中 `next_file_number 4`，只有 `sst_000002.data`）。
- 因此这份 write benchmark 测的不是引擎的稳态行为，而 1M 规模下必然触发的
  flush / compaction / 索引重建路径完全没有被覆盖。
- `block_cache_capacity = 0` 关闭了 LRU block cache，查询数字不含缓存收益
  （作为隔离测量可以接受，但需要在文档中说明）。

---

## 5. 仓库卫生

| 问题 | 细节 |
| --- | --- |
| 构建产物进入暂存区 | `git diff --cached --name-only` 共 185 个文件，其中 **170 个是 `build-*` 产物**，包含 `.o`、`.ninja_log`、可执行文件和 `stress_dbs/` 下的数据库文件 |
| `.gitignore` 覆盖不全 | 只忽略 `build/`、`/build-sanitized/`、`/build-bench/`、`/build-week5*/`、`/build-week6*/`；`build-first5*`、`build-next-stage*` 全部漏网 |
| Week 1–6 未提交 | `src/db.cc`、`src/hnsw_index.cc`、`src/vector_value.cc`、`include/*.h`、`test/*.cc` 仍在工作区；`bench/vector_bench.cpp`、`docs/WEEK6_BENCHMARK_REPORT.md`、`scripts/run_week6_benchmark.sh` 未跟踪 |
| 最后一次提交 | `b37f98f feat: add week 5 performance experiments` |
| 残留目录 | 根目录 12 个 `build-*`，以及 `.claude/worktrees/laughing-merkle-561ce8` 旧 worktree |

**一旦 commit，几百 MB 二进制会永久留在 git 历史里。**

---

## 6. 文档口径

- README 的「当前边界」是诚实的（增量索引、group commit、压缩均标注未做），无需修改。
- `VECTOR_DB_ROADMAP.md` 的 Week 6 验收标准全部标了 `[x]`，但 1M 正式跑从未执行，
  应改回未勾选或补上实测数据。
- 简历模板里的「吞吐 XX 万/s」「QPS XX 万」目前是空的 —— **不要按目标值填**，
  按实测填或先把吞吐提上去。

---

## 7. 建议的处理顺序

1. **收拾 git**（零风险、收益最大）：`git reset` 撤销构建产物的暂存，
   把 `build-first5*`、`build-next-stage*`、`tools/` 的编译产物补进 `.gitignore`，
   再把 Week 1–6 整理成一个干净的 commit。
2. **修 level 0 度数**：底层上限改为 `2 * M`，上层保持 `M`，重跑召回探针。
   这是一处小改动，能立刻量化收益。
3. **修 `level1_size_limit`**：按字节而非「条目数 × 64」推导，或直接改成可配置的
   字节阈值。
4. **改 benchmark 配置**：让 memtable 保持默认规模，使 flush/compaction 真正被覆盖；
   否则所有性能数字都不可信。
5. **决定混合检索的归属**：要么实现，要么把路线图那三条从「必须实现」移到「延后」。
6. **重测后再更新性能数字**，同步修正 README、路线图和简历描述。

---

## 8. 复现方式

```bash
# Release 构建
cmake -S . -B build-week6-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-week6-bench --parallel

# 端到端向量 benchmark
./build-week6-bench/vector_bench --mode all --path /tmp/vb_run \
    --vectors 20000 --dimension 768 --queries 200 --recall-queries 20 \
    --mixed-operations 5000 --write-batch 256 --query-batch 16 \
    --ef-search 128 --reset

# HNSW 图本身（无 LSM）
clang++ -std=c++17 -O2 -I include tools/hnsw_recall_probe.cpp \
    build-week6-bench/libkvdb.a -o /tmp/hnsw_recall_probe
/tmp/hnsw_recall_probe recall 20000 768 16 200
/tmp/hnsw_recall_probe recall 20000 768 32 200
/tmp/hnsw_recall_probe insert 20000 768 16 200
```

`tools/hnsw_recall_probe.cpp` 目前未接入 CMake，按需自行加 target。
