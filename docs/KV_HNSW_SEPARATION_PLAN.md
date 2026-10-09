# KV 与 HNSW 分离实施计划

日期：2026-10-09  
状态：待实施；本文中的模块、接口与构建命令均为目标设计，不代表当前已实现。  
主目标：把 HNSW 从 KV 核心中分离，保持 KV 主线行为与数据语义，向量检索作为可选扩展逐步演进。

## 1. 范围与完成定义

同一仓库内完成依赖、接口、文件生命周期、构建和测试的分离。暂不拆成多个仓库，也不引入通用插件框架。

完成时必须满足：

- KV 独立提供 Open/Close、Put/Get/Delete、WriteBatch、Snapshot、Iterator、Compact 和 Stats。
- WAL、MemTable、SSTable、Manifest/CURRENT、MVCC、后台 Flush、分层 Compaction、缓存和校验仍按 KV 原有语义工作。
- KV 核心不包含 HNSW 类型、向量编码/距离计算或向量配置，不调用索引维护逻辑。
- 默认构建为纯 KV；显式启用扩展后才构建向量库、HNSW、对应测试与工具。
- 关闭扩展后，旧库中的向量编码仍可作为普通字节通过 KV 读取、更新、删除、扫描和恢复。
- 索引文件缺失、损坏或不可写，不影响纯 KV 打开、恢复和维护。
- 扩展继续支持向量读写、精确搜索、HNSW 搜索、批量搜索、更新和删除，以及已有 Snapshot 视图的搜索。
- 旧持久化格式有实际兼容测试；新写入使用普通 KV 操作。
- 所有阶段都有可构建、可验证的交付物，最终通过两种构建模式的验收。

“保持 KV 主线”要求保留其行为和算法，不要求扩展 API 继续位于 DB 中。源代码调用迁移与磁盘数据兼容分别处理。

不属于本轮完成条件：多段 HNSW、并行构图、百万向量目标、混合检索、索引增量日志、生产级低延迟启动。现有 HNSWGraph 工作保留，算法优化不混入本次分离。

## 2. 当前耦合清单

以下来自当前工作区代码检查：

| 位置 | 当前职责混合 | 分离动作 |
| --- | --- | --- |
| `include/db.h` | Options 含向量参数；DB 暴露向量 API 并持有 HNSWIndex | 向量配置和 API 移到 VectorDB |
| `src/db.cc` Open/Recover | 创建索引、加载索引、解析向量 WAL | 核心只恢复字节；扩展打开后自行重建 |
| `include/memtable.h`、`src/memtable.cc` | Put/Delete 标记索引删除；PutVector 插入索引 | MemTable 只负责版本化字节和 tombstone |
| `include/write_batch.h`、`src/write_batch.cc` | 专用 PutVector API、操作类型 3、向量校验 | 新批次用 Put；旧类型仅作格式兼容读取 |
| `src/db.cc` Flush/Compaction | 生成侧挂索引、重建并替换共享图、补入并发写入 | 删除索引步骤，保留 KV 发布和版本校验 |
| `include/manifest.h`、`src/manifest.cpp` | SSTableMeta 持有 index_path，Manifest 序列化索引路径 | 旧字段在解析兼容层消费；新记录不再发布索引引用 |
| `include/format.h`、`src/format.cc` | 核心格式工具提供 HNSW 文件名 | 索引文件命名迁移到扩展层 |
| `CMakeLists.txt` | kvdb 无条件编译 HNSW 和 vector_value | 拆分库与条件构建 |
| `test/kv_test.cc` | 混合 KV、向量、持久化及召回用例 | 按行为拆分，保留原有 KV 覆盖 |
| `bench/vector_bench.cpp`、探针 | 依赖 DB 的向量接口 | 基准改用扩展，纯算法探针仅依赖 HNSW |

原基线 `289efab` 记录在 `KV_BASELINE.md`。当前未提交的 HNSW 图拆分、CMake、README、Docker、探针和文档必须先登记差异；不删除、不覆盖，也不默认当成本次分离的成果。

## 3. 目标模块与依赖

```text
KV 应用 ───────────────────────────> kv_core

向量应用 ──> vector_db ──> kv_core
                      └─> hnsw ──> vector_value

kv_core: WAL / MemTable / SSTable / Manifest / MVCC / Iterator / Compaction
vector_db: 向量接口 / 写入协调 / 索引状态 / 重建 / Snapshot 搜索
hnsw: HNSWIndex / HNSWGraph
vector_value: 向量编码解码 / 距离计算
```

HNSW 可以复用 Status、SequenceNumber 等基础类型头文件，但不得依赖 DB、MemTable、SSTable 或 Manifest，也不得通过链接 kv_core 才取得基础工具实现。实际需要的公共工具按用途提取，避免反向依赖。

建议第一版保留现有文件路径，新增 `include/vector_db.h`、`src/vector_db.cc`、`include/vector_write_batch.h`、`src/vector_write_batch.cc` 和独立测试。先完成职责分离，再评估目录搬迁，减少无关 diff。

构建目标：

| 目标 | 用途 |
| --- | --- |
| `kv_core` | 纯 KV 实现 |
| `kvdb` | 兼容原 KV 链接名称的 alias，仅指向 kv_core |
| `vector_value` | 扩展使用的编码与距离计算 |
| `hnsw` | 可独立测试的索引算法与索引序列化 |
| `vector_db` | 依赖 kv_core 和 hnsw 的扩展入口 |

新增 `KV_ENABLE_VECTOR_EXTENSION`，最终默认 OFF。SIMD、距离计数和召回回归选项仅作用于扩展目标。显式要求召回回归却关闭扩展时，在配置阶段给出明确错误。

## 4. API、所有权与一致性设计

### 4.1 KV 接口

保留现有 KV 方法签名和 Options 中的 KV 参数。移出 PutVector、GetVector、BruteForceSearch、Search、SearchBatch、VectorIndexStats 和向量参数。

核心 WriteBatch 只生成 Put/Delete。向量内容由扩展编码为普通 value，不改变 KV 对任意字节值的处理。包括看起来像向量编码的普通 value，KV 都不解析、不拒绝。

移动 Options 字段时更新所有调用点；不靠条件编译让同一个 DB/Options 在两种模式下呈现不同定义。检查聚合初始化和参数位置，避免参数静默错位。

### 4.2 扩展接口

`VectorOptions` 组合 KV Options 与维度、距离度量、HNSW 参数。`VectorDB` 通过组合持有一个 DB 和独立索引，不继承 DB。

`VectorDB` 保留普通 KV 的转发操作及向量接口，便于同一个实例存储普通值和向量。`VectorWriteBatch` 支持普通 Put/Delete 和 PutVector 混合批次，最终一次性提交为核心 WriteBatch。

第一版不暴露可写的底层 DB 句柄。该实例上的写入必须经过扩展协调，否则直接 KV 写入会绕过索引。只读转发可保留。相同目录上的另一个写实例仍属于目录锁收尾问题，不能把“不暴露句柄”当作多进程隔离。

源代码迁移：`DB::PutVector` 等改为 `VectorDB`；`WriteBatch::PutVector` 改为 `VectorWriteBatch`。如确需兼容包装，它只能位于扩展头文件，核心不保留 HNSW 依赖。

### 4.3 写入与索引失败

以 KV 为唯一事实来源。正常写入顺序：

1. 在提交前校验所有 key、向量编码、维度、有限值和距离度量要求。
2. 持扩展协调锁，一次性提交普通 KV WriteBatch。
3. KV 成功后更新索引；普通值覆盖向量、删除、同 key 多次操作都按批次顺序处理。
4. 完成索引更新后，解除协调锁，使搜索看到完整批次。

提交前的确定性错误不得写 WAL。索引错误不得进入核心 DB 的 background_status，也不得阻断核心 Flush/Compaction。

必须区分“KV 已提交”和“索引可用”：

- 扩展维护 Closed、Opening、Ready、Dirty、Rebuilding 等状态，具体枚举在实现时落实。
- KV 已提交但索引更新失败时，返回包含独立 KV 状态与索引状态的扩展写入结果，并将索引标记 Dirty；不能仅返回模糊的失败使调用方误以为数据未写入。
- 提交结果无法确定的底层错误单独标记为 Unknown，不能承诺未写入；保守地使索引失效。
- Dirty 状态下 HNSW 搜索返回明确的索引不可用错误。精确搜索和直接读取可从 KV 执行；不能静默返回旧图结果。
- 允许通过显式 RebuildIndex 恢复 Ready。普通 KV 调用继续使用原 Status 约定。
- 如保留便捷写入接口，必须在文档中解释其对复合写入结果的映射，不能丢失“已提交”信息。

第一版使用简单的扩展读写协调锁，优先保证正确性。索引内部仍负责自己的并发安全；不在本轮引入异步索引水位或并行写者。

### 4.4 Snapshot 与搜索

KV Snapshot 的 sequence、Iterator 可见性和 Compaction 版本保留规则维持原样。

扩展必须保留已有历史视图行为，第一版可以采用：

- 当前视图搜索使用维护中的 HNSW 索引。
- 显式 Snapshot 搜索从该 Snapshot 的 KV Iterator 构建临时索引，再执行 HNSW 查询；批量查询复用同一次临时索引。
- BruteForceSearch 直接扫描对应 KV 视图，作为精确校验基线。

这样不要求核心公开历史版本集合，也不把旧版本回收通知接回 Compaction。代价是历史搜索构建成本较高，需记录实测；后续独立优化。

保留更新、删除、普通值覆盖之后的历史可见性。临时重建前后都检查 Iterator 状态，失败时不发布部分结果。Snapshot 所属 DB、释放与 Close/Reopen 的有效性需要明确测试；新增校验保持有效调用行为，原有生命周期缺陷仍按 KV 收尾计划处理。

### 4.5 重建与维护

打开流程：打开 KV → 获取一致视图 → 扫描并构建新图 → 原子发布 → Ready。失败时释放 Snapshot/Iterator 并清理扩展初始化状态，允许确定的关闭和重试。

重建期间第一版阻止扩展写入，保持视图与发布一致；后台 KV Flush/Compaction 可以继续，因为它们不改变逻辑内容。

Compaction 不再更新图。持续更新/删除导致图的无效节点积累时，由扩展自己的显式重建或可配置阈值触发回收，不依赖 SSTable 数量和层级。增加长期更新测试，避免把无限积累当成分离完成。

锁顺序固定为扩展协调锁 → KV 公共 API 或 HNSW 公共 API。核心不回调扩展；不同时长期持有 KV 内部锁和 HNSW 锁。Snapshot/Iterator 必须在底层 DB 销毁前释放。

## 5. 持久化兼容与文件归属

### 5.1 WAL / WriteBatch

新向量操作编码为现有普通 Put，不改变 WAL envelope、CRC、batch count 或 sequence 分配。

旧操作字节类型 3 在核心解码兼容层识别为历史 Put 别名，保留 key/value 原字节并规范化为普通 Put。校验操作长度、批次结构和 CRC；向量语义校验交给扩展。

核心公开操作枚举不继续提供可写的 kPutVector。兼容常量限制在解码实现中，未知类型仍拒绝。同 key 混合操作的顺序、批次计数和 sequence 必须保持。

测试使用旧版本生成的 fixture 或按旧格式构造的固定字节，不能仅用新编码器自写自读证明旧库兼容。旧向量内容语义错误时，核心可读取原字节，扩展明确报告编码错误；不能把该行为变化隐藏掉。

### 5.2 SSTable / Manifest

SSTable 数据布局和 value 编码不改变。

旧 Manifest 的可选索引路径字段由解析兼容层读取并忽略，正常校验剩余字段、记录边界和 CRC。最终 SSTableMeta 不持有活跃索引字段，新 VersionEdit 不生成索引引用。

缺失或损坏的 SSTable、WAL、Manifest 仍按核心原来的错误语义处理；只能忽略扩展索引文件，不能放宽 KV 数据完整性校验。

迁移后的纯 KV 可以打开旧库。旧二进制能否反向打开新库必须用测试验证，不能据此承诺双向兼容；回退依赖未修改的测试副本/备份。

### 5.3 索引文件

第一版不要求持久化 HNSW 图，扩展每次打开从 KV 重建。旧 `index_*.hnsw` 原地保留，KV 不校验、不维护、不自动删除它们。

后续若加入索引快照，放在扩展自己的子目录，并包含格式版本、配置、视图/数据标识和完整性信息。仅有 sequence 水位不足以证明覆盖数据一致；不匹配则重建，不向 KV Manifest 增加索引依赖。

禁止核心通用“孤儿文件清理”误删扩展目录。旧索引清理工具属于单独工作，先列出候选文件再执行，不作为本轮隐式动作。

## 6. 分阶段实施与验收

### 阶段 0：记录当前基线与保护工作区

- 登记当前 revision、已有 diff、未跟踪文件和 HNSWGraph 实现状态。
- 验证当前工作区 Debug/Release 与默认 CTest；已有基线记录不能替代当前结果。
- 提取旧 WAL、Manifest、SSTable 的小规模兼容 fixture。
- 记录核心用例清单与固定参数 KV benchmark；所有分离阶段与该清单比较。

交付：基线记录、兼容样本和变更边界。验收：明确哪些内容已有、哪些属于此次修改，样本可被当前版本读取。

### 阶段 1：拆测试并提取算法库

- 编码/距离、HNSW 单测、VectorDB 集成测试分组；先保持调用行为。
- 提取 vector_value、hnsw 库；kvdb 暂时可以依赖它们，标记为过渡状态。
- 算法探针链接 hnsw，测试用公共 helper 按需提取，避免复制整份 KV 测试。
- 向量性能选项只作用于对应库，保留现有图拆分代码。

交付：可独立验证的算法库和分类测试。验收：旧测试能力保留，算法测试无需 DB；此阶段不能宣称纯 KV 已独立。

### 阶段 2：建立扩展入口与失败语义

- 新增 VectorOptions、VectorDB、VectorWriteBatch、扩展写入结果和索引状态接口。
- 实现向量编码、普通 KV 提交、索引维护、精确搜索及一致视图重建。
- 在切换核心前用扩展路径覆盖普通值混写、更新、删除和 Snapshot 搜索。
- 新增索引失败注入点，证明 KV 已提交时的结果和 Dirty 行为正确。

交付：可测试的上层扩展。验收：不依赖 DB 内部成员；不能通过保留核心索引把新接口包装成表面分离。

### 阶段 3：切断核心运行时耦合

- 移除 DB/MemTable 的索引成员和向量 API；普通 Put/Delete 不再触发 HNSW。
- Open/Recover 只恢复 KV；旧向量操作在解码层规范化。
- Flush 只创建、打开和发布 SSTable。
- Compaction 删除索引构建、侧文件发布与索引替换，保留输入有效性检查、MVCC/tombstone 裁决、Manifest 发布回滚和文件 pin。
- 检查原有 `vector_index_` 身份比较是否承担生命周期保护；需要的保护改用 KV 状态/生命周期标识，不能直接删除有效防护。
- 清除核心头文件对向量头文件的传递依赖。

这些紧密相关修改作为一次完整切换验证，不提交半个可见的写入流程。

交付：无运行时索引依赖的 KV。验收：纯 KV 读写任意字节；维护不创建或访问索引文件；核心用例全部通过。

### 阶段 4：完成格式和构建边界

- Manifest 旧字段兼容读取，新写入不再记录索引路径；核心移除索引命名函数。
- 建立 kv_core 与 kvdb alias，启用真正的扩展开关，最终默认 OFF。
- 关闭扩展时，向量源文件、测试、工具不进入构建图；没有隐藏的链接依赖。
- 迁移 vector_bench、调用示例和扩展测试，替换“每 SSTable 一个索引文件”等旧实现断言。
- 对搬走的核心测试逐项核对，保留适用的 MVCC/恢复/并发断言。

交付：两种明确的构建模式与旧库兼容。验收：纯 KV 可独立链接；旧库即使移走全部 `.hnsw` 文件仍可恢复、Compact、再打开。

### 阶段 5：生命周期、并发与资源验证

- 覆盖 VectorDB 重复 Open/Close、未打开操作、打开失败重试、Close 后搜索、Reopen。
- 验证并发写/搜索、SearchBatch 同一视图、重建/写入、Close/搜索的交错。
- 验证索引更新中途失败后不返回部分批次结果，重建后恢复正确搜索。
- 验证持续更新/删除与图回收；Snapshot 搜索在 Compaction 前后返回相同历史数据。
- 进程级测试 KV 提交后、索引更新前退出，重启从 KV 恢复全部可见向量。

交付：边界和故障测试。验收：ASan/UBSan/TSan 无报告；失败索引不污染核心状态，资源释放确定。

### 阶段 6：CI、文档与最终审查

- CI 分别构建纯 KV 和扩展；核心门禁覆盖 Debug/Release 和 sanitizer，扩展有单独测试结果。
- README 给出纯 KV 默认命令、扩展启用命令、调用迁移及故障语义。
- 更新 KV_BASELINE 与收尾计划；旧 HNSW 架构文档注明后续提案性质和当前边界。
- 同条件重复 KV benchmark，对波动或退化给出数据解释；记录扩展重启及历史搜索成本。
- 检查 diff、文件清单、依赖图和干净源码构建，无生成物或无关文件混入。

交付：可独立交付的纯 KV 和可选向量扩展。验收：第 9 节全部完成并有证据链接。

## 7. 回归与故障测试矩阵

| 领域 | 必须证明的行为 |
| --- | --- |
| KV 普通数据 | 空值、二进制值、大值、类似向量编码的值均可存储；批次顺序与删除正确 |
| MVCC | Snapshot 旧值、tombstone、Iterator、Compaction 保留规则不变 |
| 后台维护 | Flush、层间 Compaction、失败传播和文件 pin 不依赖索引 |
| 旧格式 | 类型 3 WAL、混合批次、旧 Manifest 索引字段、旧 SSTable 可读，sequence 不改变 |
| 格式损坏 | 尾部截断、CRC 错误、未知操作仍按原 KV 校验规则处理 |
| 索引隔离 | 旧索引缺失/损坏/不可写、扩展初始化失败不影响纯 KV |
| 扩展写入 | 同 key 多操作、混合值覆盖、删除、非法向量提交前拒绝 |
| 扩展失败 | KV 状态与索引状态可区分；Dirty 不返回旧图结果；重建恢复 |
| 扩展视图 | 当前、历史 Snapshot、批量查询均不暴露半批次 |
| 生命周期 | Open/Close/Reopen、失败重试、Snapshot 释放、重建临时对象无悬空引用 |
| 崩溃 | KV 提交后索引未更新即可退出；重启重建反映 KV 已恢复内容 |
| 并发与资源 | 写/搜/重建/关闭交错通过 TSan；持续更新/删除有图回收路径 |
| 算法 | 使用固定数据/种子验证召回与精确基线；历史重建拓扑变化不冒充逐位一致 |
| 构建边界 | OFF 时核心和消费端无向量对象/符号依赖；ON 时所有扩展工具可构建 |

旧 KV 生命周期与完整崩溃矩阵中尚未修复的问题继续进入 KV_CLOSURE_PLAN，不借分离宣称全部工程收尾已完成。由分离触碰或引入的失败必须在本轮修复。

## 8. 验证命令与证据

以下命令在构建开关实现后执行；结果写入单独的分离验收记录。

```sh
cmake -S . -B build-kv-debug -DCMAKE_BUILD_TYPE=Debug -DKV_ENABLE_VECTOR_EXTENSION=OFF
cmake --build build-kv-debug --parallel
ctest --test-dir build-kv-debug --output-on-failure

cmake -S . -B build-vector-debug -DCMAKE_BUILD_TYPE=Debug -DKV_ENABLE_VECTOR_EXTENSION=ON
cmake --build build-vector-debug --parallel
ctest --test-dir build-vector-debug --output-on-failure
```

两种模式分别在独立目录重复 Release、`KV_SANITIZERS=address,undefined` 与 `KV_SANITIZERS=thread`，不同 sanitizer 不共用构建目录。最终在包含所有新增源码的干净源副本验证，不能漏掉未跟踪的必要文件。

依赖证据使用 CMake 构建图、编译命令和核心静态库的符号检查，并构建一个只包含 db.h/write_batch.h、只链接 kv_core 的消费端。搜索命中用于辅助核查，不能单独证明分离。

关键头文件与核心源码检查不应再出现活跃 HNSW 类型、向量 API 和索引维护。旧格式兼容实现可以含历史字段注释/操作常量，但不得调用向量解码或索引 I/O。

KV 性能比较固定机器、模式、数据规模、batch、MemTable、层阈值、fsync 口径；至少重复 3 次记录中位数和波动。未解释的明显退化阻止完成，不能用向量性能提升代替 KV 结果。

## 9. 最终完成检查表

- [ ] 当前工作区基线、已有改动与测试清单已记录。
- [ ] kv_core 默认独立构建，kvdb alias 保持普通 KV 链接入口。
- [ ] DB、Options、MemTable、WriteBatch 无活跃向量依赖。
- [ ] Flush/Compaction/Open/Recover 不维护或访问 HNSW。
- [ ] 普通 KV API、MVCC、批次、恢复、Iterator、维护行为回归通过。
- [ ] 旧 WAL 和 Manifest 有独立 fixture 验证；新写入仅生成 Put/Delete。
- [ ] 移除或损坏索引文件不影响纯 KV 打开、维护、重开。
- [ ] VectorDB/VectorWriteBatch 的普通与向量操作、搜索、SearchBatch 工作。
- [ ] 历史 Snapshot 搜索保留，更新/删除/Compaction 前后验证通过。
- [ ] 写入结果能表达 KV 已提交但索引不可用，Dirty 与重建路径可验证。
- [ ] 生命周期、并发、进程退出恢复、长期更新/删除回收测试通过。
- [ ] 两种模式 Debug/Release/ASan+UBSan/TSan 与适用 benchmark 通过。
- [ ] 核心消费端、构建图和符号检查证明无隐藏扩展依赖。
- [ ] CI、README、迁移说明、验收记录与实际实现一致。
- [ ] 原有未提交内容被保留；新增文件可审查，无无关产物混入。

## 10. 执行约束与后续顺序

每个阶段记录修改范围、验证命令、结果和剩余问题；下一阶段以实际通过的代码为基础。失败时修复当前阶段，不关闭测试、不削弱断言、不把扩展依赖重新放回核心。

分离过程中保留原有文件布局、KV 算法和磁盘 envelope；只做切断耦合所需的修改。目录重排、通用 observer、异步索引、HNSWSegment、多段并行、增量快照和性能目标在分离验收之后独立推进。

主线顺序调整为：

**记录当前基线 → 拆测试/算法库 → 扩展入口 → 切断核心耦合 → 兼容与构建 → 故障/并发验证 → 分离验收 → KV 生命周期与后续收尾 → HNSW 独立优化。**

本计划细化 `KV_CLOSURE_PLAN.md` 的模块边界，不替代其 Options、崩溃恢复、Compaction 内存、多实例与完整工程验收任务。
