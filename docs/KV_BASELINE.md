# KV 主线基线

日期：2026-10-08

## 基线版本

- Git revision：`289efab` (`fix(ci): avoid libatomic dependency in week5_bench`)
- 验证方式：从该 revision 导出干净源码目录，在临时目录中重新配置和构建
- 当前工作区未提交的 HNSW 图拆分、探针和文档不属于本基线

## 已验证结果

### Debug

```text
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

结果：**6/6 通过**，总测试时间约 11.58 秒。

### Release

```text
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

结果：**6/6 通过**，总测试时间约 3.87 秒。

### ASan + UBSan

在同一份干净源码上使用 `-DKV_SANITIZERS=address,undefined` 构建并运行 CTest。

结果：**6/6 通过**，总测试时间约 18.53 秒；未观察到 ASan 或 UBSan 报告。

### TSan

在同一份干净源码上使用 `-DKV_SANITIZERS=thread` 构建并运行 CTest。

结果：**6/6 通过**，总测试时间约 38.19 秒；未观察到 TSan 报告。

默认测试包含：

- `kv_test`
- `hnsw_test`
- `week5_bench_sync_smoke`
- `vector_bench_maintenance`
- `vector_bench_smoke`
- `kv_stress_fixed_seed`

## KV 主线范围

本基线把以下能力视为 KV 主线：

- WAL、WriteBatch 和恢复
- SkipList MemTable、immutable MemTable 和后台 Flush
- SSTable、DataBlock、Bloom Filter 和 Block Cache
- Manifest、CURRENT 和文件生命周期
- MVCC、Snapshot 和范围 Iterator
- L0/L1/L2 Compaction
- 并发读写、错误传播和基础统计

向量值和 HNSW 属于扩展能力。当前基线保留已有向量集成以保证兼容，但后续 HNSW 图结构重构、探针和性能实验应独立提交，不作为 KV 收尾的阻塞条件。

## 当前工作区边界

在基线 revision 之上，工作区还有未提交内容，主要分为：

- HNSW 图拆分：`include/hnsw_graph.h`、`src/hnsw_graph.cc` 以及 `hnsw_index` 适配
- HNSW 诊断和文档：分区探针、索引设计文档、架构评审文档
- 测试和工具脚本：Docker 测试脚本、额外 CMake 诊断选项
- KV 收尾文档：`KV_CLOSURE_PLAN.md`

这些改动在进入主线前必须通过独立构建和测试，并与 KV 生命周期修复分开审查。

## 目标一完成条件

- [x] 已冻结 KV 主线和 HNSW 扩展边界
- [x] 已从干净源码验证 Debug 构建和测试
- [x] 已从干净源码验证 Release 构建和测试
- [x] 已记录基线 revision 和测试口径
- [ ] 将当前工作区整理成可独立审查的 KV 基线提交
- [ ] 将 HNSW 重构整理成独立扩展提交
- [x] 补充 ASan、UBSan、TSan 基线结果
- [ ] 清理或明确处理无关未跟踪文件

下一步进入目标二：统一 `Open`、`Close`、读接口和 Snapshot 的生命周期语义。
