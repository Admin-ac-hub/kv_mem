# KV 主线工程收尾计划

## 目标

先把项目收敛成一个边界清楚、可恢复、可测试、可交付的单机 LSM KV 引擎。

HNSW 是扩展能力，暂时只要求保持可编译、可测试和与 KV 生命周期兼容。混合检索、并行索引构建、百万向量性能不作为 KV 主线完成条件，待 KV 基线稳定后再单独推进。

## 一、冻结范围并建立基线

KV 主线包含：

- WAL、WriteBatch 与崩溃恢复
- SkipList MemTable、immutable MemTable 和后台 Flush
- SSTable、DataBlock、Bloom Filter、Block Cache
- Manifest、CURRENT 和文件生命周期管理
- MVCC、Snapshot、范围 Iterator
- L0/L1/L2 Compaction
- 并发读写、错误传播、统计信息和测试工具

先整理出一个可独立交付的 KV 基线提交，并记录以下基线结果：

- 干净 checkout 可以完成 Debug 和 Release 构建
- 默认 CTest 全部通过
- ASan、UBSan、TSan 构建和测试可以运行
- 工作区没有构建产物、临时数据库和无关生成文件
- HNSW 重构代码与 KV 基线分开提交，避免未完成扩展阻塞 KV 发布

## 二、修复 API 生命周期语义

重点处理 `DB::Open`、`DB::Close`、读接口和 Snapshot 的状态一致性。

- `Close()` 后 `Get`、`NewIterator`、`BruteForceSearch` 等读接口统一返回 `IOError`
- 未打开或已关闭的 DB 不允许创建 Snapshot
- 明确 `Close()` 后同一个对象是否允许再次 `Open()`；若允许，必须清理上一生命周期的 Snapshot、后台状态和统计状态
- 防止跨 DB 使用 Snapshot
- 对重复 `Open`、重复 `Close`、打开失败后重试、`Close -> Open` 和关闭期间读写补充测试
- 保证 Iterator 和 Snapshot 的生命周期关系有明确文档

验收标准：所有公共 API 在打开、关闭、重复打开和失败恢复状态下都有确定的返回值，不依赖未定义的内部状态。

## 三、补齐 Options 和文件系统错误边界

增加统一的 `Options` 校验，并保证公共 API 不把底层异常直接抛给调用方。

- `memtable_entries_limit`、`block_size`、各层阈值不能为零
- Bloom Filter 参数、向量相关配置和文件格式边界需要提前校验
- 数据库路径必须是可用目录，路径冲突和权限错误返回 `Status`
- `create_directories`、`directory_iterator`、文件删除和重命名等 filesystem 操作统一使用错误码或异常转换
- 所有配置错误必须在 WAL 写入前返回

验收标准：非法配置和无权限目录不会产生半初始化 DB，也不会留下难以判断的异常。

## 四、完成崩溃恢复闭环

围绕 WAL、SSTable、Manifest 和 CURRENT 建立故障矩阵。

需要覆盖：

- WAL 记录写入中断和尾部截断
- WAL fsync 后、MemTable 更新前后的进程退出
- MemTable 切换后、Flush 发布前后的进程退出
- SSTable 临时文件写入、fsync、rename 前后的进程退出
- Manifest append 中断
- CURRENT 替换中断
- 老 WAL 删除中断
- Compaction 输出发布前后的进程退出

同时补充：

- WAL sequence 单调性、重复记录和范围校验
- Manifest sequence 与 WAL sequence 的一致性校验
- `.tmp`、孤儿 SSTable、孤儿索引文件的启动清理策略
- 失败后重新打开只能看到已经确认提交的数据
- Manifest 引用的每个文件都存在且可校验

验收标准：每个发布步骤都能明确回答“崩溃后保留什么、忽略什么、如何继续写入”。

## 五、收紧 WriteBatch 和 Compaction

### WriteBatch

- 保证批次在成功返回前整体可见
- 处理 WAL 已成功但后续 MemTable 操作失败的情况
- HNSW 插入失败不能破坏 KV 批次语义
- 对超大 key、value、batch 和整数边界增加验证

### Compaction

- 保留现有 MVCC、tombstone 和 Snapshot 语义
- 输出按目标文件大小切分，避免完整结果长期驻留内存
- 明确 L0/L1/L2 的选择、触发和停止条件
- 旧文件删除失败时保留可恢复状态并记录明确错误
- Flush、Compaction、索引文件写出失败时清理临时产物
- 增加大数据量、长 Snapshot、持续更新和持续删除测试

验收标准：Compaction 的内存占用和文件数量随数据规模可控，失败后不会留下无法解释的持久化状态。

## 六、补充并发和多实例边界

- 同一路径第二个 DB 实例显式拒绝，建议增加目录锁
- 测试写入、读取、Flush、Compaction 和 Close 的交错时序
- 测试后台 Flush 失败、Compaction 失败和 WAL 清理失败后的错误传播
- 用 TSan 覆盖长时间并发读写、后台维护和关闭流程
- 确认 Iterator、SSTable pin 和旧文件删除之间没有生命周期竞态

验收标准：并发错误不会静默丢数据，后台错误能稳定传递给后续调用方。

## 七、建立 KV 验收门禁

完成后固定执行：

1. 干净 checkout 的 Debug 构建和测试
2. 干净 checkout 的 Release 构建和测试
3. ASan + UBSan 测试
4. TSan 测试
5. 固定种子压力测试多轮运行
6. 进程级崩溃恢复测试
7. WAL、Manifest、SSTable 解码器的模糊测试或随机损坏测试
8. KV 写入、点查、范围扫描、Compaction 的重复 Benchmark
9. 工作区和提交内容检查，确保没有生成物和无关文件

KV 主线完成的判定条件：

- 所有上述测试通过
- Close 后 API 行为一致
- 同目录多实例有明确拒绝行为
- 崩溃恢复不丢失已确认写入
- 失败路径没有持续产生孤儿文件
- Compaction 在大数据量下不会无界增长内存
- README、测试、Benchmark 和实际实现口径一致

## 八、HNSW 后续路线

KV 基线完成后，再按独立阶段推进：

1. HNSW 图结构和索引持久化整理
2. 增量索引维护与 Flush/Compaction 协调
3. Recall、延迟和构建吞吐的正式 Benchmark
4. 索引版本回收和并行构建
5. metadata 过滤、范围条件和混合检索
6. 百万向量规模验收

每个 HNSW 阶段都需要单独的正确性测试、性能数据和文档，不把目标值当成已完成能力。

## 执行顺序

**仓库整理 → 生命周期 → Options/异常 → 崩溃恢复 → WriteBatch/Compaction → 并发与多实例 → CI 与验收门禁 → HNSW 扩展。**
