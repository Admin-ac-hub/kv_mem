# Benchmark Analysis

这份文档用于解释 `kv_bench` 的 workload、指标含义和结果分析方法。它不是为了证明这个项目已经达到生产级 KV 引擎性能，而是为了把 benchmark 变成可复现、可解释的工程证据。

## Workloads

`bench/kv_bench.cpp` 支持以下模式：

| 模式 | 行为 | 适合观察 |
|---|---|---|
| `--write N` | 顺序写入 `bench_key_0 ... bench_key_{N-1}`，value 为固定大小 payload | WAL fsync、MemTable 插入、Flush/Compaction 对写入的影响 |
| `--read N` | 顺序读取 `bench_key_0 ... bench_key_{N-1}` | 顺序点查路径、Block Cache 和 SSTable index 效果 |
| `--read-random N` | 使用指定 seed 随机读取 `bench_key_i` | 随机点查、Bloom Filter、Block Cache 命中情况 |
| `--scan N` | 创建 `NewIterator()`，从 `SeekToFirst()` 开始顺序 `Next()`，最多扫描 N 条 | Iterator 范围扫描吞吐 |
| `--mixed N` | 偶数轮写入，奇数轮读取已有 key，近似 50% 写 + 50% 读 | 读写混合场景下的平均吞吐和延迟 |

`scripts/run_benchmarks.sh` 默认在独立的 `build-bench/` 中用 Release 配置只构建 `kv_bench`，然后为每个场景准备独立 DB 目录。读和扫描场景会静默写入 100K 条准备数据，再在同一目录上运行对应 workload；脚本只输出五组正式测量结果。

## Metrics

`kv_bench` 输出的字段含义如下：

| 字段 | 含义 |
|---|---|
| `writes` | 本轮 benchmark 执行的写操作数量 |
| `reads` | 本轮 benchmark 执行的点查数量 |
| `scan_items` | iterator 扫描出的条目数量 |
| `elapsed_sec` | 本轮 workload 墙钟耗时，单位秒 |
| `qps` | 每秒处理的操作数；scan 模式下分母是 `scan_items`，其他模式下分母是输入 N |
| `avg_latency_us` | `elapsed_us / operations` 得到的平均单操作耗时，单位微秒 |
| `sstables` | benchmark 结束时 DB 当前可见 SSTable 数量 |
| `compactions` | benchmark 期间触发的 compaction 次数 |
| `cache_hits` | Block Cache 命中次数 |
| `cache_misses` | Block Cache 未命中次数 |
| `bloom_filtered` | Bloom Filter 判断 key 一定不存在、从而跳过 SSTable 查询的次数 |
| `block_reads` | 实际读取并解码 DataBlock 的次数 |
| `db_size_bytes` | DB 目录内所有普通文件长度之和，不是文件系统实际分配的物理空间 |

几个 caveat：

- `avg_latency_us` 是整轮平均值，不是 p95/p99 tail latency。
- read benchmark 中 `NotFound` 会被当作成功读处理，方便衡量查询路径本身。
- `db_size_bytes` 会包含 WAL、Manifest、SSTable 等文件，并受文件系统行为影响。
- 结果受机器、磁盘、文件系统、后台负载和 OS page cache 影响；比较结果时应记录硬件、编译器、commit 和运行次数，不把单次数字当作固定性能承诺。

## 如何解释结果

### 写入

写入路径需要先把 batch 追加到 WAL 并 fsync，然后再写入 active MemTable。MemTable 达阈值后会切换成 immutable，由后台线程 flush 成 SSTable。因此写入吞吐主要受 WAL 持久化成本、MemTable 插入成本，以及后台 Flush/Compaction 是否跟得上影响。

当前 benchmark 是单线程逐条写入，不能展示 group commit 的收益。如果后续实现多个 writer 聚合成一次 WAL fsync，写吞吐和平均延迟会是值得重点对比的指标。

### 顺序读和随机读

点查路径会先查 active MemTable 和 immutable MemTable，再按新到旧查询 SSTable。进入 SSTable 后，会依次经过 Bloom Filter、内存 block index、Block Cache 和 DataBlock 解码。

顺序读通常更容易复用相邻 key 所在的 DataBlock，因此 Block Cache 更容易发挥作用。随机读更容易打散访问位置，如果 cache 未命中，就需要更多 block 读取；如果查询 miss 较多，Bloom Filter 可以直接过滤掉确定不存在的 SSTable，减少无效 block lookup。

### 范围扫描

范围扫描通过 `NewIterator()` 创建多个 child iterator，并由 heap-based MergingIterator 做顺序归并。它避免了每个 key 都重新走一次完整点查路径，因此顺序扫描通常比逐 key 点查减少更多重复过滤和定位工作。

这里不应简单理解成“纯内存扫描”。实际是否触发磁盘读取取决于数据规模、OS page cache、Block Cache 和 SSTable block 访问情况。更准确的说法是：范围扫描受益于顺序遍历和 iterator 归并，减少了逐 key 点查的重复过滤和定位成本。

### 混合读写

mixed 模式交替执行写和读，平均结果同时包含 WAL 持久化成本和读路径过滤/缓存收益。它适合作为“普通服务端负载”的粗略 smoke benchmark，但不能替代更真实的多线程读写压测。

## 瓶颈和趋势

- **WAL fsync 限制写吞吐**：单条写入逐次持久化时，写入吞吐容易被 fsync 成本限制。
- **SSTable 数量影响读放大**：SSTable 越多，点查越可能需要检查更多文件；Compaction 通过合并文件降低读放大，但会引入额外写放大。
- **Bloom Filter 主要优化 miss-heavy 查询**：当 key 明确不存在某个 SSTable 时，可以跳过 index/block 读取；但 false positive 仍需要继续查。
- **Block Cache 优化热点 block**：热点 key 或顺序访问复用同一批 DataBlock 时，cache hit 可以减少重复读取和解码。
- **Compaction 输出会占用峰值内存**：当前输入端使用 SSTable iterator 和 heap 归并，但输出端仍会收集 entries 后调用 `CreateFromEntries`。

## 如何复现

构建：

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --config Release --target kv_bench
```

单独运行：

```bash
./build-bench/kv_bench --path ./bench_db --write 100000 --value-size 100
./build-bench/kv_bench --path ./bench_db --read 100000
./build-bench/kv_bench --path ./bench_db --read-random 100000 --seed 1
./build-bench/kv_bench --path ./bench_db --scan 100000
./build-bench/kv_bench --path ./bench_db --mixed 100000 --value-size 100
```

推荐一键运行：

```bash
./scripts/run_benchmarks.sh
```

脚本会删除并重建 `bench_runs/`，避免旧数据污染不同场景。

## 后续工作

Benchmark 方法和项目级优化统一维护在 [README 的“后续优化方向”](../README.md#后续优化方向)。
