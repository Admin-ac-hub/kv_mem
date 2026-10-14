# KV / HNSW 分离验收记录

日期：2026-10-09。实施计划：[KV_HNSW_SEPARATION_PLAN.md](KV_HNSW_SEPARATION_PLAN.md)。

> 本文保留模块分离阶段的接口、测试数量和性能测量。随后完成的目录锁、配置校验、持久化失败及编号边界修复见 [KV 主线收尾计划](KV_CLOSURE_PLAN.md)；新增回归后测试数为纯 KV 6 个、扩展 12 个，本文性能数字未包含后续边界修复。

已完成同仓库模块分离，默认构建纯 KV；显式启用扩展后构建 VectorDB、编码/距离和
HNSW。KV 的 WAL/MemTable/SSTable/Manifest/MVCC/后台维护算法保留，核心不再调用
向量解码或索引维护。此记录不代表 KV 收尾计划中的目录锁、全 I/O 故障矩阵或
生产级向量启动性能已经完成。

## 基线与变更边界

用户先提交工作区为 `1da617a`（`chore: checkpoint KV and HNSW separation baseline`），
然后实施本轮修改。分离前当前源码已验证 Debug **6/6**（13.98 秒）、Release
**6/6**（3.65 秒）。原冻结主线 `289efab` 的记录继续保留在 `KV_BASELINE.md`。

已有 HNSWGraph 拆分、分区探针、Docker 脚本和设计文档均已纳入该 checkpoint。
本轮保留图算法和 HNSW v1/v2 文件格式，仅调整基础工具依赖、构建归属及相关说明。
已有 SVG、简历和其他无关文档未修改。本轮没有替用户创建新的实现提交。

原 `kv_test.cc` 的 **47 个 Test 函数全部保留**：纯 KV 保留 40 个，6 个集成测试
移到 `vector_db_test.cc`，编码/距离测试移到 `vector_value_test.cc`。额外在纯 KV
加入大字节值 Compaction 预算测试和生命周期测试，避免把原大向量字节预算覆盖
仅留在扩展里。原“每 SSTable 生成/删除 HNSW 文件”断言改为“不生成新文件、保留
旧侧文件”，图回收由显式/阈值重建验证。

## 最终模块与实现

| 目标 | 依赖与职责 |
| --- | --- |
| `kv_common` | 无 DB/索引依赖；Status、固定宽度编码、CRC32、fsync |
| `kv_core` / `kvdb` alias | kv_common + Threads；纯字节 KV、MVCC、存储维护 |
| `vector_value` | kv_common；向量编码、校验和距离/SIMD |
| `hnsw` | vector_value + Threads；图、版本可见性及独立索引序列化 |
| `vector_db` | kv_core + hnsw；扩展接口、写入协调、视图重建与失败状态 |

新增 `kv_common` 是计划中“提取公共基础工具”的落实：HNSW 不通过链接 KV 才取得
Status/CRC/fsync，也不复制这些实现。`DB`/`Options` 在两种构建模式下定义一致。
`KV_ENABLE_VECTOR_EXTENSION` 默认 OFF；召回回归开启而扩展关闭会在配置时失败。
SIMD 和距离计数只作用于扩展目标。

VectorOptions 组合 Options，VectorDB 组合持有 DB，不暴露可写底层句柄。
VectorWriteBatch 编码成普通 WriteBatch Put/Delete；带向量魔数的普通 Put 在扩展中
也校验并索引，以保证打开重建与在线写入采用相同解释。所有确定性校验在提交前
完成；协调锁覆盖整批 KV 提交和索引更新。同 key 多次操作按批次顺序更新图。

`VectorWriteResult` 独立提供 kv_status、index_status、NotCommitted/Committed/Unknown。
KV 成功、索引失败时保留 Committed 并进入 Dirty；HNSW 搜索拒绝返回旧图，精确搜索
和普通读取继续使用 KV。底层写错误保守标记 Unknown；显式 RebuildIndex 可恢复图。
普通 Put/Delete 的 Status 错误消息明确包含提交状态，需要结构化结果时使用 Write。

锁顺序为 lifecycle → coordination → KV 或 HNSW 公共 API。Close/Open 使用独占
lifecycle；Compaction 只持共享 lifecycle，不持 coordination，所以构建期间可以
写入和搜索。重建持独占 coordination，KV 后台维护仍可进行。临时 Iterator pin
一个一致 KV 视图，历史批量搜索复用同一临时图，不暴露部分构建结果。

索引内部 sequence 仅用于该图的操作排序，重建后重新从 0 排序，不宣称它是可持久化
的 KV 水位。历史可见性来自 KV Snapshot Iterator。默认每 4096 次操作重建，回收
历史节点与 tombstone；设 0 可关闭自动回收。重建完全独立于 SSTable 数量和层级。

原 Compaction 的索引身份比较替换为 KV lifecycle_generation 比较，输入存活检查、
Manifest 发布回滚、文件 pin 与 MVCC/tombstone 裁决继续保留。
同步补充 Get/Iterator 关闭状态检查、Snapshot 所属/释放/跨 Open 校验。释放的小句柄
保留到 DB 析构，避免原始指针地址重用；已释放句柄不再参与历史版本保留。大量创建
Snapshot 的句柄内存随 DB 生命周期累积，是当前原始指针 API 的明确代价。

## 格式兼容与故障证据

[固定样本说明](../test/fixtures/README.md)和
[原版本样本生成源码](benchmark_results/20261009-separation/make_legacy_fixture.cc)。

- `legacy_vector` 来自分离前实际二进制：旧 SSTable、带索引字段的 Manifest、HNSW
  侧文件以及类型 3 / Put / Delete 的混合 WAL；恢复后 sequence 为 9。
- `legacy_invalid_vector` 用独立固定格式构造，类型 3 携带语义错误的 `KVV1broken`。
  核心保留字节并支持恢复/扫描/更新/删除；扩展明确返回编码错误，修正 KV 后可重试打开。
- 核心解码把旧类型 3 规范化为 Put，公开枚举和新编码不提供类型 3。未知类型、长度
  溢出/截断、CRC 损坏继续拒绝。Manifest 旧索引字段校验后忽略，新记录没有索引引用；
  旧 number/name-only edit 仍可读取，开始但不完整的 metadata 被拒绝。
- 旧索引缺失、损坏、只读以及只读扩展子目录均不阻碍纯 KV Open/Compact/Close/Reopen；
  侧文件内容保留。实际 SSTable/WAL/Manifest 损坏仍失败。
- 批次索引中途失败、索引异常、失败重建和底层写错误分别验证提交状态、Dirty、精确
  读取及恢复。损坏最后一个 SSTable data block 后，精确搜索、历史临时图和重建均拒绝
  发布部分结果；修复文件后显式重建恢复。
- Snapshot 更新/删除/普通值覆盖及 Compaction 后历史可见性不变；释放/跨库/跨 Open
  Snapshot 被拒绝。并发批量搜索中的所有查询使用同一完整批次视图；并发重建、写入、
  Close/搜索通过 sanitizer。持续更新/删除 256 轮后图节点受阈值约束，显式重建后只剩活跃节点。
- `posix_spawn` 启动子进程，WAL 成功提交后、索引更新前 `_exit(73)`；父进程重新打开
  后从 KV 恢复完整两条向量。该测试也在 ASan/UBSan/TSan 构建中执行。

另使用分离前二进制读取一份新格式普通 Put/SSTable/Manifest 库，读取与 HNSW 查询
成功：[结果](benchmark_results/20261009-separation/reverse-compatibility.txt)、
[旧读取端源码](benchmark_results/20261009-separation/reverse_reader.cc)。这是小规模
反向兼容实验，不构成对所有旧二进制/库状态的双向兼容承诺；回退仍使用备份。

## 构建与 sanitizer 验证

本机 macOS 27.0 / arm64 / AppleClang 21.0。每种配置独立构建目录；未观察到
ASan、UBSan 或 TSan 报告。以下最终回归曾并行运行，总耗时不能拿来比较性能。

| 配置 | 纯 KV | 扩展开启 |
| --- | --- | --- |
| Debug | [5/5](benchmark_results/20261009-separation/debug-off.txt) | [11/11](benchmark_results/20261009-separation/debug-on.txt) |
| Release | [5/5](benchmark_results/20261009-separation/release-off.txt) | [11/11](benchmark_results/20261009-separation/release-on.txt) |
| ASan + UBSan | [5/5](benchmark_results/20261009-separation/asan-off.txt) | [11/11](benchmark_results/20261009-separation/asan-on.txt) |
| TSan | [5/5](benchmark_results/20261009-separation/tsan-off.txt) | [11/11](benchmark_results/20261009-separation/tsan-on.txt) |

典型命令（替换 ON/OFF、构建类型与 sanitizer 即可）：

```sh
cmake -S . -B build-kv-debug -DCMAKE_BUILD_TYPE=Debug -DKV_ENABLE_VECTOR_EXTENSION=OFF
cmake --build build-kv-debug --parallel
ctest --test-dir build-kv-debug --output-on-failure

cmake -S . -B build-vector-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DKV_ENABLE_VECTOR_EXTENSION=ON -DKV_SANITIZERS=thread
cmake --build build-vector-tsan --parallel
ctest --test-dir build-vector-tsan --output-on-failure
```

包含全部新增源码和二进制 fixture、排除 `.git`/构建目录/临时数据库的干净源码副本
额外验证默认 Debug **5/5** 与扩展 Release **11/11**：
[结果](benchmark_results/20261009-separation/clean-source.txt)。

[依赖审计](benchmark_results/20261009-separation/dependency-audit.txt)检查 OFF 编译命令
没有向量源文件、libkv_core.a 没有 HNSW/编码/距离符号，并实际构建只包含
`db.h/write_batch.h`、只声明链接 kv_core 的核心消费端。
[核心构建图](benchmark_results/20261009-separation/core-dependencies.dot)、
[扩展构建图](benchmark_results/20261009-separation/vector-dependencies.dot)和
[互斥配置失败结果](benchmark_results/20261009-separation/negative-recall.txt)作为附加证据。
算法测试链接命令没有 kv_core。`git diff --check` 与修改脚本的 `bash -n` 通过。

GitHub Actions 已配置 Ubuntu/macOS × Debug/Release × OFF/ON；sanitizer 也各自
覆盖两种模式。远程 CI 尚未触发，本地未执行可选 20k 慢速召回回归；该回归继续在
扩展 Release CI 开启，已有固定种子 HNSW 与小规模集成召回测试均通过。

## 同条件 KV 性能与扩展成本

以同一份[普通 KV benchmark 源码](benchmark_results/20261009-separation/core_benchmark.cc)
分别链接旧 Release libkvdb 与新 Release libkv_core/kv_common。AppleClang 21.0，
`-O3 -DNDEBUG -std=c++17`，在其他构建/测试结束后交替运行旧/新各 3 次，使用新目录。
固定 10000 个 key、128 字节 value、batch=32、MemTable=256、L0 limit=4、L1=65536
字节、block cache=64、逐批 WAL fsync，读随机种子 20261009。写入耗时包含 Close
的 Flush/Compaction 收尾，之后重开执行 10000 次随机 Get 和完整扫描。
每轮均为 **40 次 Flush / 16 次 Compaction**。

[全部原始数据](benchmark_results/20261009-separation/core-performance.json)。下表单位秒：

| 工作负载 | 旧中位数（min–max） | 新中位数（min–max） | 中位耗时变化 |
| --- | --- | --- | --- |
| 批量写入含收尾 | 0.179218（0.172191–0.210008） | 0.178172（0.176448–0.246597） | -0.58% |
| 随机 Get | 0.254934（0.252483–0.258934） | 0.253254（0.250570–0.270835） | -0.66% |
| 完整扫描 | 0.009882（0.009811–0.010107） | 0.010180（0.009717–0.010187） | +3.01% |

写入首轮存在 fsync/文件系统与调度波动；扫描只有约 10 ms，两组范围重叠，3% 对应
约 0.3 ms。未观察到超出这些样本波动的明显退化，不能据三次小负载声称普遍性能提升。

扩展成本独立测量：1000 向量、8 维、M=16、ef_construction=200；每批 8 个查询，
TopK=10、ef=64。三轮开启、更新并查询历史 Snapshot，批量历史查询复用一个临时图：
[源码](benchmark_results/20261009-separation/lifecycle_cost.cc)、
[结果](benchmark_results/20261009-separation/lifecycle-cost.txt)。

| 操作 | 中位耗时 |
| --- | --- |
| Open 扫描 KV 并重建图 | 34.129 ms |
| Snapshot SearchBatch（含临时构图） | 33.969 ms |
| 当前 SearchBatch | 0.0526 ms |

历史查询的构图开销与 Open 同量级，明显高于复用当前图；本轮按计划接受这一成本，
后续独立优化索引快照或历史图缓存，不将依赖接回核心 Manifest/Compaction。

## 分阶段交付对应

| 阶段 | 可审查交付与验证 |
| --- | --- |
| 0 基线 | checkpoint 1da617a、分离前 Debug/Release、固定旧样本与 47 项覆盖清单 |
| 1 算法/测试 | vector_value/hnsw 独立库，测试分类，算法链接审计 |
| 2 扩展 | VectorDB/VectorWriteBatch、提交结果、Dirty/重建与历史查询测试 |
| 3 核心切换 | DB/MemTable 纯字节，Open/Recover/Flush/Compact 无索引维护，KV generation 防护 |
| 4 格式/构建 | 旧类型 3/Manifest 兼容、默认 OFF、独立消费端和无索引文件维护验证 |
| 5 资源/并发 | 故障/生命周期/长期回收/进程退出测试与两种模式 sanitizer |
| 6 文档/验收 | CI 双模式、README 迁移、干净源码、依赖图、三轮性能与成本记录 |

本轮改动保留在工作区供审查。后续 KV 收尾按 `KV_CLOSURE_PLAN.md` 继续，分段 HNSW、
并行构图、索引持久化/增量日志与百万向量目标仍是独立后续工作。
