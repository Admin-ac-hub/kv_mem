# KV 主线工程收尾计划

更新：2026-10-09。当前目标是冻结可复现、能解释的纯 KV 基线，用于一周后的面试准备。
本文件记录已验证边界和后续待办；后续待办不是面试前必须完成的承诺。

## 当前范围与状态

KV 主线包含 WAL/WriteBatch、SkipList MemTable、后台 Flush、SSTable/Bloom/Cache、
Manifest/CURRENT、MVCC/Snapshot/Iterator 和简化 L0/L1/L2 Compaction。
默认 `KV_ENABLE_VECTOR_EXTENSION=OFF`，核心只存字节；可选 VectorDB/HNSW 独立构建。
模块分离和兼容证据见 [分离验收记录](KV_HNSW_SEPARATION_REPORT.md)。该记录里的
5/11 个测试是分离时的历史结果；新增 core_boundary_test 后当前默认为 6 个，扩展为 12 个。

| 边界 | 当前实现与证据 |
| --- | --- |
| 生命周期 | Open/Close 串行，重复 Open 拒绝，重复 Close 允许，同对象可重开；关闭后新读/写/Compact 拒绝，空批次也检查状态 |
| Snapshot | 检查所属 DB、有效状态及 Open 周期；释放句柄保留到析构，但不保留历史版本 |
| Iterator | 固定创建时读视图，SSTable 按块加载，MemTable 复制；已有 Iterator 可在 Close 后读 pin 的视图，应在 DB 销毁前释放 |
| Options | 路径非空；MemTable/L0/L1/block 阈值为正，block_size 不超过 uint32；Bloom bits/key 为 1–64，cache=0 禁用 |
| 路径与实例 | 路径冲突通过 Status 返回；LOCK/flock 拒绝第二个协作实例，失败 Open/Close 释放锁，不删除 LOCK 文件 |
| 写入边界 | key 不为空且不含 tab/newline/NUL；长度、总 payload、sequence 和文件号范围检查在写前执行；值允许任意字节 |
| 持久化错误 | WAL I/O 或 Manifest 发布错误阻止后续写入；结果可能已提交，Close/Open 后确认；不复用不确定 WAL 的 sequence |
| 发布与文件 | Manifest 发布错误保留新 SSTable，正常有 Manifest 时忽略孤儿文件；恢复保留孤儿文件编号以免覆盖 |
| 格式恢复 | 支持超过六位的文件号；WAL 序号范围、Manifest 必需字段/本地文件名/层级范围、载荷长度及部分整数溢出检查 |
| 后台与读取 | Flush/Compaction 错误传播；Get 按 sequence 裁决；Iterator/读请求 pin 已打开 SSTable，Compaction 删除后可继续读 |

实现入口：[db.cc](../src/db.cc)、[wal.cc](../src/wal.cc)、[manifest.cpp](../src/manifest.cpp)、
[core_boundary_test.cc](../test/core_boundary_test.cc)。LOCK 使用现有 POSIX 平台能力，未增加 Windows 支持。
目录锁为协作式锁；不要在实例存活时删除 LOCK、替换数据库目录或外部修改文件。

## 本轮检查与验收

新增回归覆盖非法/零配置、路径冲突与失败重试、关闭状态空批次、进程间锁与路径别名、
并发 Open/Close、零序号 Snapshot、WAL 同步错误后恢复整批、Manifest 不确定发布保留文件、
长文件编号/孤儿编号、重开缓存、序号重叠/溢出、巨大声明长度及部分非法元数据。
原 Flush 测试已改为分别等待“发布 SSTable”和“清理 WAL”，二者存在合法异步窗口。

本机 macOS arm64，AppleClang 21.0；最终源码回归结果如下，未观察到 Sanitizer 报告。
历史冻结基线见 [KV_BASELINE.md](KV_BASELINE.md)。各配置独立构建目录，测试耗时不用于性能比较。

| 配置 | 模式 | CTest | 原始输出 |
| --- | --- | --- | --- |
| Debug | 纯 KV | 6/6 | [debug-off](benchmark_results/20261009-kv-boundaries/debug-off.txt) |
| Release | 扩展开启 | 12/12 | [release-on](benchmark_results/20261009-kv-boundaries/release-on.txt) |
| ASan + UBSan / Debug | 纯 KV | 6/6 | [asan-off](benchmark_results/20261009-kv-boundaries/asan-off.txt) |
| ASan + UBSan / Debug | 扩展开启 | 12/12 | [asan-on](benchmark_results/20261009-kv-boundaries/asan-on.txt) |
| TSan / Debug | 扩展开启，包含全部 KV 回归 | 12/12 | [tsan-on](benchmark_results/20261009-kv-boundaries/tsan-on.txt) |

复现默认纯 KV：

```bash
cmake -S . -B build-kv-review -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-kv-review --parallel
ctest --test-dir build-kv-review --output-on-failure
```

Release 改 `CMAKE_BUILD_TYPE=Release`；ASan/UBSan 用独立目录加
`-DKV_SANITIZERS=address,undefined`，TSan 用独立目录加 `-DKV_SANITIZERS=thread`。
扩展模式另加 `-DKV_ENABLE_VECTOR_EXTENSION=ON`。本地结果不代表远端 CI 已执行。

## 必须保留的失败语义

成功写入先完成 WAL fsync，再进入 MemTable。错误返回可能发生在写入 WAL 之后或
MemTable 更新之后，不能据此认定未提交；完整 WAL 可能在重开后恢复出来。
当前没有通用 KV CommitState API，调用方需要重开读取并使用自身幂等策略确认结果。

当前测试覆盖记录完整性、尾部截断、故障注入、并发时序及分离时的一个进程退出场景。
进程退出不等于机器断电；没有覆盖所有 SSTable/Manifest/CURRENT 发布点的断电矩阵。
有 Manifest 时以发布版本为准；Manifest/CURRENT 都缺失时的目录扫描是兼容路径，
可能纳入孤儿表，不作为任意元数据丢失后的安全恢复保证。

## 一周内执行范围

1. 固定当前源码与构建结果，面试前只修可复现的阻塞问题。
2. 对齐简历、README 和 [面试核心素材](interview_core.md)，不保留过时接口或目标性能数字。
3. 准备读写/恢复图、Snapshot 与 tombstone 手推、一个真实并发 bug 和一个失败恢复案例。
4. HNSW 仅作为附加能力，准备基本搜索、M/ef 参数、精确召回基线和与 KV 的分离边界。

## 后续工程待办

- 完整 I/O 故障及进程退出/断电发布矩阵，WAL/Manifest/SSTable 解码器模糊测试。
- 内存分配失败与其他非 filesystem 异常的强边界；当前不承诺所有异常都转 Status。
- 流式 SSTable Builder、目标文件大小切分、完整 Compaction Picker 与块压缩。
- immutable 队列背压、大数据与长 Snapshot 内存/空间控制；输出目前全量物化。
- Snapshot 裸指针句柄管理改进；释放句柄的数量随 DB 对象生命周期累积。
- 启动孤儿文件清理及删除失败治理；核心不得清理扩展目录或历史侧索引。
- writer queue、group commit、可配置 durability、Manifest 滚动和长期稳态 Benchmark。

上述任务按真实需求独立推进。本轮不推进分段 HNSW、并行构图、混合检索或百万向量验收。
