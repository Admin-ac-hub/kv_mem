# 第五周：性能与未定义行为实验报告

## 目标与结论

本周把“性能更好”拆成可复现的数字，并确认优化没有引入未定义行为（UB）。
在当前 Mini LSM-KV 中，最主要的真实性能成本是持久化路径：每次
`DB::Write` 都会调用一次 `WALWriter::AppendBatch`，并在返回前执行 `fsync`。
把多个操作放进同一个 `WriteBatch` 可以把这项固定成本摊薄；在本次机器和
workload 上，batch size 32 的写入吞吐约为单条 `Put` 的 9.1 倍。

这不是把 durability 关闭后得到的数字：每个 `DB::Write` 仍然执行一次 WAL
flush + fsync。批处理减少的是 fsync 次数，同时保留一个 batch 的原子提交语义。

## 实验环境

| 项目 | 值 |
| --- | --- |
| 主机 | Apple Silicon `Mac16,1`，10 CPU，16 GiB |
| 系统 | macOS 26.6.2，Darwin 25.6.0 |
| 编译器 | AppleClang 21.0.0 (`-O3`，CMake `Release`) |
| 构建 | CMake + Ninja，C++17 |
| 源码基线 | `6bbccd7` 加本地未提交改动 |
| 测量日期 | 2026-09-03 |

DB 测量使用固定 100-byte value、100,000 次操作、单个 writer；计时从第一
个写操作开始，到最后一个 `DB::Write` 返回结束，不包含 `Open` 和 `Close`。
为避免后台 flush 进入计时，`memtable_entries_limit` 设为操作数加一；
`block_cache_capacity` 设为 0。同步原语实验使用 1 个 worker、5,000,000 次
递增操作来隔离原语自身成本；将 `SYNC_THREADS` 设为 4 可另外观察争用。所有
模式最后都检查计数器确实等于操作数。

## 结果

命令：

```bash
OPERATIONS=100000 SYNC_OPERATIONS=5000000 SYNC_THREADS=1 BATCH_SIZE=32 VALUE_SIZE=100 \
  ./scripts/run_week5_experiment.sh
```

### 同步原语

| 模式 | ns/op | ops/s | lock-free | 说明 |
| --- | ---: | ---: | --- | --- |
| `mutex` | 4.05 | 247.21 M | false | 每次递增都进入互斥锁临界区 |
| `atomic` | 1.60 | 626.56 M | true | `fetch_add(relaxed)`；运行时检查 `is_lock_free()` |
| `batched-mutex` | 0.37 | 2.68 B | false | 每 32 次递增合并一次锁内更新 |

在这个无争用、纯内存计数器上，atomic 约为 mutex 的 2.5 倍；批量合并临界区
约为 mutex 的 10.8 倍。`batched-mutex` 不是无锁算法，它只是减少了共享状态
访问次数；它仍需要锁来发布每一批结果。真实系统中不能只看这张表：atomic 的
内存序、缓存一致性和失败重试都会随共享变量数量与争用模式变化。

### DB 写入

| 模式 | batch size | ns/op | ops/s | 相对单条 `Put` |
| --- | ---: | ---: | ---: | ---: |
| `db-put` | 1（每次构造一个 batch） | 30,588.0 | 32,692.5 | 1.00x |
| `db-batch` | 1 | 34,673.9 | 28,840.1 | 0.88x |
| `db-batch` | 32 | 3,360.5 | 297,577.0 | 9.10x |

batch size 1 的差异属于同一持久化策略下的测量噪声；只有 batch size 增大后，
fsync 次数显著下降，吞吐才出现数量级变化。脚本每次使用独立临时目录，避免
旧 WAL/SSTable 数据复用；OS page cache 和机器背景负载仍可能影响结果。
上表是一次运行的记录，正式比较时应重复运行并报告中位数，而不是把单次数字
当作固定性能承诺。

## 性能问题定位

macOS 没有本机 `perf` 和 `gdb`，本次使用带调试信息的构建配合系统
`sample` 采样；Linux 上可直接用 `scripts/profile_week5.sh` 的 `perf stat -d`
分支。采样命令：

```bash
OUTPUT=/tmp/kv-week5-sample.txt OPERATIONS=100000 \
  ./scripts/profile_week5.sh
```

采样结果中，主线程调用链稳定落在：

```text
DB::Put -> DB::Write -> WALWriter::AppendBatch -> FsyncFile -> fsync
```

2 秒采样的 140 个主线程样本里，118 个位于 `FsyncFile`，其中 83 个在系统
`fsync`；对应源码是 `src/wal.cc:105` 和 `src/format.cc:153`。这与 DB
batch 对比相互印证：问题不是单纯的 `std::string` 或 SkipList 分配，而是
每次小写入都付出一次持久化固定成本。

## UB、实现定义与未指定行为检查

- **UB**：标准不要求任何结果，例如有符号溢出、越界访问、使用已释放对象。
  本周用 `address,undefined` 构建跑完整 CTest；`kv_test`、`hnsw_test`、固定
  seed 压力测试和 `week5_bench_sync_smoke` 均通过，没有报告 ASan/UBSan 错误。
- **实现定义行为**：实现必须选择并记录结果，例如 `char` 是否有符号。二进制
  格式的字节写入统一通过 `static_cast<char>` 写入、读取时通过
  `static_cast<unsigned char>` 解码（`src/format.cc`），不把 `char` 的符号性
  当作数值语义。
- **未指定行为**：实现可以在多个合法结果中选择，例如未规定的求值顺序。新增
  benchmark 不依赖参数求值顺序；共享计数器只通过 mutex 或 atomic 访问。

Sanitizer 验证命令：

```bash
cmake -S . -B build-week5-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DKV_SANITIZERS=address,undefined
cmake --build build-week5-asan --parallel
ctest --test-dir build-week5-asan --output-on-failure -j2
```

## 可复现文件

- `bench/week5_bench.cpp`：同步原语与 DB batch 对比，输出 CSV。
- `scripts/run_week5_experiment.sh`：Release 构建并运行四个场景。
- `scripts/profile_week5.sh`：Linux 使用 `perf stat -d`，macOS 使用 `sample`。

## 下一步

如果服务端允许把多个独立请求合并提交，应在 writer queue 中实现有界 group
commit，并明确最大等待时间、失败传播和 durability 配置；不能把本报告的单线程
batch 数字直接当作多线程服务吞吐承诺。Compaction 输出端仍有一次性 entries
vector，后续可用 allocator/流式 builder 实验它的内存峰值，再与本周的 WAL 固定
成本分开比较。
