# Week 6 Vector Benchmark Report

## Scope

**Acceptance status: not passed.** No 1M × 768 formal result has been produced.
The development baseline below predates the September 16 fixes and is retained
as historical evidence, not a measurement of the current implementation.
Current repair measurements, including a maintained 6k × 768 write run, are in
[the September 16 review follow-up](REVIEW_FIXES_20260916.md).

Week 6 adds the `vector_bench` executable and a repeatable runner for three
vector workloads:

| Workload | Default configuration | Measured outputs |
| --- | --- | --- |
| Write | 1,000,000 generated Float32 vectors, 768 dimensions | throughput, P50/P99 per-vector-equivalent batch latency, SSTable/HNSW/WAL bytes |
| Query | 1,000 random queries against 1,000,000 persisted vectors | QPS, P50/P99 per-query-equivalent batch latency, sampled exact Recall@10 |
| Mixed | 1,000,000 operations: 50% batched writes and 50% batched reads | independent write throughput, query QPS, query P50/P99, storage bytes |

The suite uses a deterministic generated corpus and query noise, so each seed
recreates the same data without retaining a multi-gigabyte source dataset in
memory. Query mode first builds and closes the database, then reopens it before
timing the HNSW reads. This includes persisted-index recovery in the workload
setup but excludes setup time from the query measurement.

`--recall-queries` controls how many of the measured queries receive an exact,
full-corpus Top-K comparison. Exact recall is intentionally separate because it
is O(queries * vectors * dimensions). The default runner validates 10 queries;
set `RECALL_QUERY_COUNT=1000` to report Recall@10 for all 1,000 queries.

## Optimizations

- Distance calculation has an AVX2 implementation selected by runtime CPU
  detection on x86 GCC/Clang builds, plus a double-accumulation NEON backend on
  AArch64. Other architectures retain the scalar path. `distance_backend`
  identifies the selected backend; `-DKV_ENABLE_SIMD=OFF` forces scalar for
  comparisons. The historical results below used scalar on arm64.
- HNSW nodes store coordinates in one contiguous `vector_data_` array. Nodes
  carry offsets instead of owning independently allocated vector buffers.
- Layer traversal uses a thread-local contiguous generation-marker array rather
  than allocating and zeroing a visited bitmap for each layer search.
- `HNSWIndex::SearchBatch` and `DB::SearchBatch` validate a group of queries and
  hold the HNSW shared lock once for the group.

## Reproduction

```bash
./scripts/run_week6_benchmark.sh
```

This uses the formal 1M x 768 defaults. It requires a fresh selected directory
under `bench_runs/`, preserves earlier runs, and writes environment/build details
plus one text result per workload. CMake and Ninja are resolved from PATH or
Homebrew; `CMAKE_BIN` and `NINJA_BIN` override their locations. For a short run:

```bash
VECTOR_COUNT=2000 DIMENSION=128 QUERY_COUNT=100 RECALL_QUERY_COUNT=10 \
MIXED_OPERATIONS=1000 RUN_ROOT="$PWD/bench_runs/example-new-run" \
./scripts/run_week6_benchmark.sh
```

Use a Release build, idle host, local SSD, and at least three repetitions for a
comparison. Write throughput includes deterministic vector generation and
`DB::Write` and `Close()` (draining flush/compaction and index builds);
write latency percentiles measure only `DB::Write` batch time,
normalized by its vector count. Query timing uses `SearchBatch`; its displayed
percentile is batch elapsed time divided by the number of queries in that batch,
so it is a per-query-equivalent latency, not a separately scheduled request
latency.

Current defaults retain the engine's 1,024-entry MemTable, L0 limit 4, L1 byte
budget 64 MiB and block-cache capacity 64. Override these with `MEMTABLE_LIMIT`,
`L0_LIMIT`, `L1_BYTES`, and `BLOCK_CACHE`, or the corresponding CLI switches.
Setting the MemTable above the corpus size explicitly measures an ingest phase
with only a final flush; it is unsuitable as a steady-state result. The old
baseline did exactly that and also disabled the block cache. ANN traversal uses
the in-memory graph, so enabling the SSTable block cache does not itself speed
up that traversal.

Each workload prints `flush_count`/`compaction_count`. The CTest
`vector_bench_maintenance` requires both paths to run. `--require-maintenance`
fails a run that does not exercise them. `--hnsw-m` and `--ef-construction`
make index settings explicit. `*_batch_amortized_p50_us/p99_us` describe batch
averages; set `QUERY_BATCH_SIZE=1` for individually timed requests. Mixed
`mixed_wall_*_per_second` rates share one wall-clock window including Close;
the older `mixed_write/query_throughput_per_second` outputs are phase service
rates, not simultaneously achieved workload rates. Mixed mode is sequential
alternation, not concurrent readers/writers.

## Development Baseline

This historical baseline proves the measurement program ran on the development
machine. It bypassed normal maintenance during ingest and excluded Close from
write timing; it does not establish steady-state or 1M x 768 performance.

- System: macOS 26.6.2, arm64
- Compiler: Homebrew Clang 22.1.6, default `build/` configuration
- SIMD: AVX2 unavailable on arm64; scalar fallback selected
- Dataset: deterministic 2,000 x 128 Float32 vectors; 100 queries; exact
  Recall@10 sampled across 10 queries; `M=16`, `ef_construction=200`,
  `ef_search=128`, write batch 64, query batch 16

| Workload | Write throughput | Query QPS | Query P99 | Recall@10 | SSTable bytes | HNSW bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Write | 827.25/s | n/a | n/a | n/a | 1,116,029 | 1,444,208 |
| Query | n/a | 1,557.08/s | 660.54 us | 0.9900 | 1,116,029 | 1,444,208 |
| Mixed (50/50) | 711.15/s | 1,774.07/s | 611.94 us | n/a | 837,100 | 1,083,996 |

The development run does not meet the roadmap's 1M x 768 performance targets,
and no such claim is made here. In particular, AVX2 cannot run on this arm64
machine and the current write path still serializes HNSW insertion under an
exclusive lock. Run the formal suite on an AVX2-capable x86 host before using
the targets as demonstrated metrics.
