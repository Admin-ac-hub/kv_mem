# HNSW Index Persistence Redesign — Global Index Snapshot

> **Superseded by [`VECTOR_INDEX_ARCHITECTURE.md`](VECTOR_INDEX_ARCHITECTURE.md) (2026-09-28).**
> That document re-examines the problem and proposes a larger refactor; this one addressed only
> the restart problem and left the write path serialised. Kept for the record.
>
> Note: the `docs/zh/` references below are stale — that directory was removed on 2026-09-28.

Status: **proposal, not implemented**
Date: 2026-09-28
Supersedes: the A/B/C comparison formerly in `docs/zh/03-存储与持久化.md` §8.3.

---

## 1. Problem statement and measured baseline

### 1.1 Current layout

| Artifact | Producer | Contents |
| --- | --- | --- |
| `<n>.hnsw` | `DB::SaveVectorIndexEntries` (`src/db.cc:1458`) | A **freshly built** sub-index containing only that SSTable's entries |
| MANIFEST | `VersionEdit::sstables[].index_path` (`include/manifest.h:15`) | Per-SSTable index path |

Open path (`DB::RebuildVectorIndex`, `src/db.cc:1409`):

```cpp
Status status = first ? vector_index_->Load(meta.index_path)
                      : vector_index_->MergeFrom(meta.index_path);
```

`MergeFrom` (`src/hnsw_index.cc:839`) loads the file completely — **including the neighbour
tables** — then throws them away and re-inserts every version:

```cpp
HNSWIndex loaded(options_);
loaded.Load(path);                                   // edges are read here
for (const auto& key_versions : loaded.versions_by_key_) {
  ... InsertVersion(key, sequence, vector, metadata); // and discarded here
}
```

Compaction rebuilds the whole index from scratch (`DB::BuildVectorIndexForCompaction`,
`src/db.cc:1356`).

**Net effect: the same vectors are inserted into an HNSW graph three times** — once on the
write path, once for the flush sub-index, once for the compaction rebuild.

### 1.2 Measured baseline

All figures below are measured; conditions are stated because none of them generalise.

| Metric | Value | Conditions | Source |
| --- | ---: | --- | --- |
| End-to-end write throughput | **316.93 rows/s** | 6000×768, M=16, ef_c=200, batch=256, NEON, MemTable=1024, L0=4, L1=64 MiB, `--require-maintenance` | `docs/REVIEW_FIXES_20260916.md:74` |
| of which Close/drain | **11.69 s of 18.93 s (62%)** | same run; 6 flushes + 1 compaction, 2 SSTables | same |
| Pure HNSW insert (NEON) | 962.5 inserts/s (median of 3) | 5k×768, M=16, ef_c=200, single thread | `docs/REVIEW_FIXES_20260916.md:65` |
| Pure HNSW insert (scalar) | 379.0 inserts/s (median of 3) | same | same |
| Index bytes / SSTable bytes | **1.04×** | 65,695,164 vs 62,963,946 | `docs/VECTOR_DB_STATUS_REVIEW.md:70-72` (older baseline) |
| Slice experiment: write | 288.5 → **483.0** rows/s (1.67×) | flush sub-index changed to "copy a slice from the shared graph" | `docs/zh/02-代码结构导读.md:410` |
| Slice experiment: Close | 13.26 → **5.13** s | same | same |

Two caveats on these numbers:

- The slice experiment and the 316.93 rows/s run are **different batches** and must not be
  compared directly (the latter includes the maintenance path).
- The slice experiment only removed the **flush-side** duplicate construction. Compaction still
  rebuilt everything. It is therefore a **lower bound** on what this design can achieve, not a
  prediction of it.

### 1.3 Why the edges are dead weight

Only the first file is ever `Load`ed. For every subsequent file the neighbour tables are read
into memory and then discarded, so:

```
restart cost ≈ O((N − |file1|) × ef_c × dim)
index file size ≈ SSTable size
```

The index pays full storage cost for the vectors (a second copy of every vector, alongside the
copy already inside `.sst` via `EncodeVectorValue`) plus the edge tables, and buys back almost
nothing at restart.

---

## 2. Why the obvious fix does not work

The tempting fix — "stop re-inserting, just take over the loaded graph" — is incorrect, and it is
worth recording why so it is not attempted again.

`SaveVectorIndexEntries` constructs each sub-index from an **empty** `HNSWIndex`:

```cpp
HNSWIndex sub_index(index_options);          // empty
for (const auto& entry : entries) { ... sub_index.InsertVersion(...); }
```

Consequences:

1. **Node ids are file-local** (`0 .. n_i−1`), so concatenation requires remapping every id in
   every neighbour list.
2. **Cross-file edges do not exist.** Sub-index *A* only contains edges among *A*'s own nodes.

Concatenating the files therefore produces a graph of **mutually unreachable components**.
`SearchAtSequenceLocked` starts from a single `entry_point_`, so a query reaches only the
component that entry point happens to live in — every other component becomes unsearchable.

This is why `MergeFrom` re-inserts: it pays `O(N × ef_c × dim)` to buy back connectivity.
`ReplaceWith` (`src/hnsw_index.cc:870`) is worse still — it is a full `move` of all three
containers, so it would leave only the last file's nodes.

**The fix must change the granularity of the index file, not the merge strategy.**

---

## 3. Target design

> One global index snapshot, opened once, appended incrementally.

### 3.1 Data model change

**Before**

```
<db>/
  MANIFEST-<n>          → sst 1 → hnsw_1   (sub-index, entries of sst 1)
                          sst 2 → hnsw_2   (sub-index, entries of sst 2)
                          ...
  <n>.sst
  <n>.hnsw              (one per SSTable)
```

**After**

```
<db>/
  MANIFEST-<n>          → sst 1 → sst_1    (no index_path)
                          sst 2 → sst_2
                          ...
  <n>.sst
  hnsw.snapshot         (one global file, fixed name, atomically renamed)
```

The snapshot is **not** owned by any SSTable. It is a point-in-time image of the whole graph,
bound to a sequence watermark.

### 3.2 Snapshot file format (v3)

Reuse the v2 body layout unchanged (`src/hnsw_index.cc:496-603`). Only the header grows:

```
Header (v3)
  magic               u64
  version             u32   = 3
  dimension           u32
  max_neighbors       u32
  ef_construction     u32
  metric              u32
  max_level           u32
  node_count          u64
  version_count       u64
  entry_point         u64
  covered_sequence    u64   ← new: watermark S_H
  covered_file_count  u32   ← new: k
  covered_files       u64[k] ← new: file numbers F_H
Body
  <node records>            (unchanged from v2)
  <version table>           (unchanged from v2)
CRC32(body)           u32
```

`covered_files` is stored sorted so the set comparison on open is a single linear pass.

v2 files remain readable (they simply have no watermark and force the slow path). v3 files are
**not** readable by the old binary — same compatibility policy as the v1 → v2 upgrade
(`docs/REVIEW_FIXES_20260916.md:27`).

### 3.3 Consistency contract

> Loading snapshot *H* and then incrementally inserting every entry not covered by *H* must
> yield an index that is **equivalent** to rebuilding from the current SSTable set.

"Equivalent" here means **the same searchable data set**, not the same edge set. HNSW graph
structure depends on insertion order, so byte-identical graphs are not achievable and must not
be asserted in tests. What must hold:

1. Every live `(key, sequence)` in the SSTable set is present and visible in the index.
2. Every version dropped by compaction is a tombstone in the index.
3. `entry_point_` and `max_level_` are consistent with the loaded graph.

Property (1) is testable with a point-lookup sweep; property (2) with `GetVector` after
compaction; property (3) with `HNSWStats()`.

### 3.4 Open path

```
DB::Open()
  LoadOrRecoverManifest()                       # existing
  ├─ snapshot missing / v3-param mismatch       → full rebuild (today's behaviour)
  ├─ covered_files == current SSTable set       → Load only            (FAST PATH)
  └─ covered_files ≠ current SSTable set        → Load + insert the difference
  Recover()                                     # WAL replay, existing
```

Fast path is the **normal** case: `Close()` writes a snapshot covering exactly the SSTable set
that is live at that moment, so a clean Close → Open cycle always takes it.

The third branch covers crash recovery. Its cost is `O(ΔN × ef_c × dim)` where `ΔN` is the
number of entries written since the last snapshot — not `O(N)`. If `ΔN / N` exceeds a threshold
(proposed: 30%), fall back to a full rebuild, since incremental insertion of a large fraction is
slower than one clean rebuild.

**Ordering constraint:** the snapshot must be loaded *before* `Recover()` runs, because WAL
replay calls `InsertVersion` and needs a populated graph to attach new nodes to.

### 3.5 Write path changes

**Flush** — `FlushImmutableMemTable` (`src/db.cc:~1750`):

- Delete the `SaveVectorIndexEntries` call (`src/db.cc:1754`).
- Do **not** set `SSTableMeta::index_path`.
- Keep everything else: entries already reached the in-memory index via
  `MemTable::PutVector` → `HNSWIndex::InsertVersion` when they were written.

**Compaction** — `CompactUnlocked` (`src/db.cc:809-1197`):

- Delete the `BuildVectorIndexForCompaction` call (`src/db.cc:1027`).
- Replace it with **version reconciliation**: for every `(key, sequence)` present in the input
  tables but absent from the output, call `HNSWIndex::MarkDeleted(key, sequence)`.

`MarkDeleted` touches only `versions_by_key_` — it does not modify `nodes_` or any neighbour
list, so it is O(1) per dropped version and does not disturb graph structure. This is the
critical property that makes the whole design cheap.

**Ordering constraint:** reconciliation must run **after** the compaction is published, inside
the same `version_mu_` critical section that swaps `sstables_`. `MarkDeleted` is not reversible,
and compaction can fail before publishing — running it early would tombstone versions that are
still live.

### 3.6 Snapshot scheduling

**Phase 1 (this design): snapshot on `Close()` only.**

Rationale:

- Clean restart always takes the fast path.
- Crash recovery cost is bounded by the WAL replay window, and WAL replay is required anyway.
- `Save` takes `std::shared_lock` while writes need `unique_lock`, so saving **blocks writes**.
  At `Close()` the DB is already draining, so this costs nothing. A background snapshotter would
  need either a full copy of the graph (doubling peak memory) or a stop-the-world pause — both
  are worse than the problem they solve.

Deferred: a background snapshot every *k* flushes. Revisit only if measurements show crash
recovery time is a real problem.

### 3.7 Node reclamation

Compaction leaves tombstones behind. Tombstoned nodes **still participate in graph traversal**
(that is deliberate — see `docs/zh/01-HNSW原理.md` §7.2: excluding them would disconnect regions
reachable only through them). So tombstones accumulate:

- `node_count` grows beyond the live data set
- traversal touches dead nodes, slowing queries
- resident memory grows

Mitigation: a threshold-triggered rebuild.

```
live_node_count  = number of non-tombstoned entries in versions_by_key_
if node_count > 2 × live_node_count:      # >50% tombstones
    rebuild from all SSTable entries and ReplaceWith()
```

Trigger the check after compaction publishes, not on every write. This makes rebuilds
**amortised** rather than per-compaction: with a 50% threshold and a workload that supersedes
each key once, a rebuild happens roughly once per "half the data set has been rewritten",
instead of on every single compaction.

The threshold is a tunable; 2× is a starting point, not a measured optimum.

---

## 4. Migration

Existing databases have per-SSTable `.hnsw` files and `index_path` entries in MANIFEST.

1. On first open after upgrade, `hnsw.snapshot` does not exist → **full rebuild** from SSTables.
   This is exactly today's slow path, so it is already exercised code.
2. `Close()` writes the first snapshot.
3. Old `<n>.hnsw` files are **left in place** until a snapshot has been written and verified
   readable. Only then may they be deleted — and deletion must be opt-in, not automatic, since
   it is destructive. Until deleted they merely waste space; nothing reads them.

`SSTableMeta::index_path` is retained in the struct (marked deprecated in a comment) so that
databases written by older binaries can still be opened by the slow path.

---

## 5. Expected impact

### 5.1 Measured inputs

| Input | Value | Source |
| --- | ---: | --- |
| Removing flush-side duplicate construction | 288.5 → 483.0 rows/s; Close 13.26 → 5.13 s | `docs/zh/02-代码结构导读.md:410` |
| Close/drain share of end-to-end time | 62% | `docs/REVIEW_FIXES_20260916.md:74` |

### 5.2 Estimates (not measurements)

For N = 20,000, dim = 768, M = 16:

```
vector bytes  = 20000 × 768 × 4            ≈ 61.4 MB
edge bytes    ≈ 20000 × 16 × 8 × 1.5       ≈  3.8 MB   (level-0 dominant, ×1.5 for 2M reverse links)
snapshot      ≈ 65 MB
```

Sequential read of 65 MB at ~500 MB/s (typical NVMe) is ~0.13 s of I/O; adding deserialisation
and version-table construction, **a fast-path open is estimated at 0.2–0.5 s**. This is an
estimate derived from the byte count above, **not a measurement**, and must be replaced with a
real number before it appears in any report.

### 5.3 What this does NOT fix

Write throughput stays bounded by the serial insert path. Removing duplicate construction
addresses the `C4` item only; `C3` (inserts serialised behind `write_mu_` / `version_mu_` /
`memtable_mu_` plus the index's exclusive lock) is untouched. Even taking the slice experiment's
483 rows/s as an optimistic bound, the 50k rows/s target remains ~100× away. Do not present this
design as a throughput fix.

---

## 6. Risks and mitigations

| # | Risk | Mitigation |
| --- | --- | --- |
| 1 | Recall differs between snapshot-loaded and rebuilt graphs | Run `hnsw_recall_probe` regression against both paths at identical parameters; treat any drop beyond noise as a blocker |
| 2 | Tombstone accumulation degrades queries and memory | §3.7 threshold rebuild; expose `live_node_count` in `HNSWStats` |
| 3 | Snapshot silently inconsistent with SSTable set | `covered_files` + `covered_sequence` in the header; mismatches force rebuild rather than being tolerated |
| 4 | v3 files unreadable by older binaries | Same policy as v1 → v2; document it in the release note |
| 5 | Compaction failure after reconciliation | Reconcile only after publish, inside `version_mu_` (§3.5) |
| 6 | Rebuild peak memory = two full graphs | Trigger rebuilds only from the compaction thread, which already holds a rebuilt index in the current code |
| 7 | Incremental path slower than expected for large ΔN | 30% fallback threshold (§3.4) |

---

## 7. Implementation plan

Each stage is independently testable and leaves the tree working.

| Stage | Change | Files |
| --- | --- | --- |
| 1 | Add v3 header fields (`covered_sequence`, `covered_files`); keep per-SSTable path intact | `src/hnsw_index.cc` (`Save`/`Load`), `src/db.cc` |
| 2 | Write `hnsw.snapshot` at `Close()`; verify it loads | `src/db.cc` |
| 3 | Switch `RebuildVectorIndex` to the snapshot path with full-rebuild fallback | `src/db.cc:1409` |
| 4 | Remove `SaveVectorIndexEntries` from flush | `src/db.cc:1754` |
| 5 | Remove `BuildVectorIndexForCompaction`; add post-publish reconciliation | `src/db.cc:1027`, `src/db.cc:1356` |
| 6 | Add threshold-triggered rebuild (§3.7) | `src/db.cc`, `src/hnsw_index.cc` |
| 7 | Migration: detect legacy layout, rebuild once, document cleanup | `src/db.cc` |

Stages 4 and 5 are the ones that produce the throughput and Close-time wins; stages 1–3 are the
prerequisite for correctness.

---

## 8. Test plan

| Area | Test |
| --- | --- |
| Crash recovery | `kill -9` between flush and Close; reopen; verify every key via point lookup and a recall sweep |
| Consistency | Flush, compaction and Close interleaved; assert `covered_files` comparison forces rebuild when they diverge |
| Recall | `hnsw_recall_probe regression 20000 768 32 200 100` on snapshot-loaded vs rebuilt graphs; same parameters both sides |
| End-to-end | `vector_bench --mode write --require-maintenance` before/after; report flush/compaction counts alongside throughput |
| Boundaries | Empty DB; single SSTable; parameter mismatch (dimension / M / metric); truncated snapshot; bad CRC |
| Concurrency | Snapshot save racing with an in-flight search (ASan + TSan builds) |
| Migration | Open a v2 database, confirm one rebuild, confirm subsequent opens take the fast path |

Regression gates already in CI (`KV_ENABLE_RECALL_REGRESSION`) should be extended to cover both
open paths rather than adding a parallel harness.

---

## 9. Open questions

1. **`covered_files` vs `covered_sequence` alone.** The file-set comparison makes the fast path
   exact but breaks whenever compaction renumbers files. Would a sequence watermark plus
   per-SSTable `largest_sequence` be more robust at the cost of an extra `SSTableMeta` field?
2. **Background snapshots.** §3.6 defers them. If a long-running process never closes cleanly,
   crash recovery degrades to a full rebuild. Is that acceptable, or does the WAL replay window
   already bound it?
3. **Reclamation threshold.** 2× is a guess. It needs a measurement of query latency versus
   tombstone ratio before it can be defended.
4. **Legacy file cleanup.** Leave to the operator, or add an explicit maintenance command?
5. **`MergeFrom` disposition.** Once the snapshot path lands, `MergeFrom` is dead code. Remove
   it, or keep it for the legacy fallback path?
