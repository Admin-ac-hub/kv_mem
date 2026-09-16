# 2026-09-16 评审修复记录

本轮按“正确性、配置、测试、文档优先，再处理性能”执行。不是 A–G 全部关闭，
也不是 Week 6 百万向量正式验收。原始输出见 [运行记录](benchmark_results/20260916-review/)。
修改基于 `b37f98f` 及工作区已有的 Week 1–6 未提交代码；没有清除这些代码、构建目录或旧 worktree。

## 已修复与验证

| 清单项 | 变更与证据 |
| --- | --- |
| B1 | HNSW 新节点仍选 M 条边，反向边底层上限 2M、上层 M；各层度数有回归测试 |
| B7 | 空层结果在读取 front 前检查；加载校验入口最高层、跨层边、重复/自环边、版本映射和文件计数；坏文件不替换旧图 |
| B6 | 小规模 Recall 门限升为 >0.95；新增 20k × 128/768、每组 100 查询的慢速回归，Release CI 启用。明确使用 M=32、ef_c=200、ef=512；不声称默认参数达标 |
| D1 | L1 改为独立 `level1_size_limit_bytes`，默认 64 MiB；768 维测试分别验证默认预算留在 L1、小预算推进 L2 |
| D2/D3 | benchmark 默认 MemTable=1024、cache=64；公开可配置参数，输出 flush/compaction 次数，write 吞吐包括 Close 收尾；维护路径 smoke 强制要求真实 flush/compaction |
| A5 | `DB::VectorIndexStats()` 暴露图统计，包含各层最大度数；与轻量 `Stats()` 分开，避免每次轮询遍历整图 |
| G1/G2 | runner 支持最小 PATH、Homebrew 和工具路径覆盖；探针接入 CMake，参数/引擎错误返回非零退出码 |
| F1–F5 | 区分程序交付与性能验收；删除简历模板中未实现能力的断言；批量摊销 P99 与单请求 P99 分开，混合吞吐增加统一 wall-clock 口径 |
| C1/C2/C5、G3（部分） | 增加 AArch64 NEON 双精度累加距离后端；保留 `KV_ENABLE_SIMD=OFF` 标量对照，报告实际后端；没有宣称达到性能目标 |

额外修复了清单外的 **Get 版本顺序错误**：并发 Flush 可以先于正在构建的
Compaction 发布，旧记录所在的合并文件随后排在新记录文件之后。原 Get 按文件
顺序返回，导致旧 tombstone 遮住新 Put，或旧 Put 覆盖新 Delete。现在按可见
sequence 选择结果；固定时序测试覆盖覆盖写、删除、复活、Snapshot 和重启。
这可能增加跨 SSTable 点查次数，后续可用版本范围元数据剪枝，不能恢复依赖文件顺序的假设。

索引写出格式升为 **v2**，新程序仍能读 v1。旧程序不能读取 v2。
旧 v1 图的原有底层度数不会因为读取而自动补满；需要重建才能评估新构图策略。

## 验证结果

- macOS arm64，Apple Clang 21.0.0；Release 使用 CMake `Release`，标量对照显式关闭 SIMD。
- Release CTest **8/8**，含两个 20k 回归。
- Debug ASan+UBSan CTest **6/6**；标量、NEON、ASan 的距离参考值及更严格小规模 Recall 测试通过。
- 固定种子压力测试连续 **20 次通过**；新增并发时序测试固定复现并覆盖上述 Get 错误。
- `PATH=/usr/bin:/bin` 下 runner 完整执行 write/query/mixed，且三条路径均满足 `--require-maintenance`。
- 本机没有执行 x86 AVX2 或 Linux TSan；已配置的 CI 结果需提交后另行确认。

### 召回率

固定 L2 数据、100 个查询；i.i.d. Gaussian 查询独立生成，共享中心聚类查询
使用与数据相同的 200 个中心、独立噪声。分布外聚类查询使用独立中心，单独报告。
构图与查询参数一起决定质量/代价，不能跨参数比较后声称只有算法收益。

| 规模/后端 | M | 查询类型 | ef=128 | ef=256 | ef=512 |
| --- | ---: | --- | ---: | ---: | ---: |
| 20k × 128，scalar，修正 2M | 16 | Gaussian | 0.832 | 0.927 | 0.978 |
| 20k × 768，scalar，修正 2M | 16 | Gaussian | 0.762 | 0.876 | **0.941** |
| 20k × 128，NEON | 32 | Gaussian | 0.957 | 0.994 | 0.999 |
| 20k × 768，NEON | 32 | Gaussian | **0.874** | 0.956 | **0.992** |
| 20k × 768，NEON | 32 | 共享中心聚类 | 1.000 | 1.000 | 1.000 |
| 20k × 768，NEON | 32 | 独立中心聚类（OOD） | 0.626 | 0.742 | **0.919** |

回归门限只适用于注明参数的 Gaussian/共享中心测试；OOD 和低 ef 的不足仍保留。
这些日志是正确性诊断，部分运行期间有其他验证任务，其 QPS/构图耗时不是性能验收值。

### NEON 插入对照

相同最终 HNSW 算法、Release 编译器、固定种子，5k × 768、M=16、ef_c=200，
单线程纯 HNSW Insert。三轮交替运行 scalar/NEON，构图计时不包含生成数据。

| 后端 | 三轮 inserts/s | 中位数 |
| --- | --- | ---: |
| scalar | 416.0 / 379.0 / 365.0 | 379.0 |
| NEON | 987.5 / 647.9 / 962.5 | 962.5 |

中位数比值 **2.54×**；波动较大，应在固定硬件的空闲机器上复测。
这是距离内核带来的局部收益，不是 DB 端到端吞吐，也不能外推到 1M 或 x86。

### 包含后台维护的写入

另一次独立运行使用 6,000 × 768、M=16、ef_c=200、batch=256、NEON，
保持默认 1024 条 MemTable、L0 limit=4、L1=64 MiB，并启用 `--require-maintenance`。
实测 **316.93 条/s**，总耗时 **18.93 s**，其中 Close/drain **11.69 s**；
实际完成 **6 次 flush、1 次 compaction**，最终 2 个 SSTable。
这说明重复构图和后台收尾仍是重要成本，不能把纯 HNSW 插入收益直接当作 DB 吞吐收益。
这是单次 6k 维护路径测量，不是长期稳态或 1M 验收，亦不能直接与旧 20k 无维护路径数字比较。
原始输出见 `benchmark_results/20260916-review/write6000-maintenance.txt`。

## 需修正的原始归因

- B2：反向边使用多样性启发式本身是常见做法，不能直接判定其错误。本轮试验
  移除启发式后被淘汰邻居的回填，仅将 20k × 768 Gaussian、M=16、ef=512
  Recall 从 0.941 提至 0.946，仍未过 0.95；未把这个试验版本合入。
- B4：原聚类探针给查询生成另一组中心，属于分布外查询。结果不能排除分布影响。
- B5：一次 ef_construction 对照不足以排除构建质量因素。
- B6：1000 节点、M=16 的图并非接近全连接；问题在于小规模测试不代表大规模质量。
- F3：单次同步吞吐上限取决于 batch_size/fsync_latency；单 batch fsync 并不数学上排除
  5 万条/s，但 group commit 也无法单独解决串行 HNSW 的构建瓶颈。
- D3：SSTable block cache 不参与常驻内存 HNSW 图遍历，开启它不等同于 ANN QPS 提升。

## 尚未关闭

| 项目 | 当前状态与后续验证 |
| --- | --- |
| A1 | metadata 预过滤、范围与向量组合、分数融合仍未实现 |
| A2–A4 | 完整引擎延迟/吞吐/构建耗时/存储内存指标仍未提供；Recall 需要外部精确基线或显式抽样验证 |
| A6、B3、C1、C5 | 默认参数和正式规模尚未达标；未执行 1M × 768，不能填入目标值 |
| C3/C4/C6/C7 | 插入锁串行、重复构图、每 batch fsync、版本图常驻仍存在；下一步优先计量并减少 Flush/Compaction 重复构图，再考虑 writer queue 和版本回收 |
| D4/D5/D6 | 完整 Compaction Picker、流式输出/多文件切分、块压缩未实现 |
| E1/E2 | 前序已处理，暂存区为空；构建和数据库产物仍按 `.gitignore` 忽略 |
| E3/E4/E5 | 原有未提交工作、构建目录和旧 worktree 保留；本轮未提交或删除 |
| G3 | arm64 现在走 NEON；AVX2 专项数据仍需 x86 主机实测，不能拿 arm64 数字替代 |

## 复现

```bash
/opt/homebrew/bin/cmake -S . -B build-review-fixes -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKV_ENABLE_RECALL_REGRESSION=ON
/opt/homebrew/bin/cmake --build build-review-fixes --parallel
/opt/homebrew/bin/ctest --test-dir build-review-fixes --output-on-failure

# 默认 M=16 的不足可直接复现；该配置 regression 模式可能返回失败。
./build-review-fixes/hnsw_recall_probe recall 20000 768 16 200 100
# 较高质量配置；仍不是百万规模验收。
./build-review-fixes/hnsw_recall_probe regression 20000 768 32 200 100

/opt/homebrew/bin/cmake -S . -B build-review-scalar -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKV_ENABLE_SIMD=OFF
/opt/homebrew/bin/cmake --build build-review-scalar --target hnsw_recall_probe
./build-review-scalar/hnsw_recall_probe insert 5000 768 16 200
./build-review-fixes/hnsw_recall_probe insert 5000 768 16 200

./build-review-fixes/vector_bench --mode write --path /tmp/kv-write-fresh-path \
  --vectors 6000 --dimension 768 --write-batch 256 --require-maintenance
```

其他平台用 PATH 中的 cmake/ctest；runner 自动解析工具位置。
