# 向量数据库改造计划

## 项目定位

将现有的 Mini LSM-KV 改造为**轻量级向量数据库**，在保留 LSM-Tree 优秀写入性能和崩溃恢复能力的基础上，增加高效的向量存储和近似最近邻（ANN）搜索能力。

**目标场景**：
- 语义搜索（文本 embedding 检索）
- 推荐系统（用户/商品向量相似度）
- RAG 应用（文档向量检索）
- 图像/音频相似度搜索

**核心价值主张**：
- **高吞吐写入**：继承 LSM-Tree 的批量写入优势，适合实时更新的向量数据
- **混合检索**：支持向量相似度 + 标量过滤（比如 `WHERE category='tech' AND similarity > 0.8`）
- **持久化可靠**：完整的 WAL + Manifest + 崩溃恢复
- **轻量部署**：单机版，无复杂依赖，可嵌入应用

---

## 最终功能边界

### 核心功能（必须实现）

#### 1. 向量存储
- [x] 现有：key-value 存储（LSM-Tree）
- [x] 新增：支持 `key → (vector, metadata)` 映射
- [x] 向量维度：支持固定维度（128/256/512/768/1024 等常见维度）
- [x] 距离度量：欧氏距离（L2）、内积（IP）、余弦距离
- [x] 数据格式：向量 + JSON metadata（标签、时间戳、分类等）

#### 2. HNSW 索引
- [ ] 实现分层可导航小世界图（Hierarchical Navigable Small World）
- [ ] 索引构建：增量构建，支持并发插入
- [ ] 索引持久化：SSTable 同步刷写索引文件
- [ ] 参数可调：`M`（每层最大边数）、`ef_construction`（构建时搜索宽度）
- [ ] 内存管理：索引常驻内存或 mmap

#### 3. ANN 搜索
- [ ] TopK 查询：返回最近的 K 个向量
- [ ] 查询参数：`ef_search`（搜索时的动态列表大小）
- [ ] 结果返回：`(key, distance, metadata)` 列表
- [ ] 性能目标：100 万向量，P99 延迟 < 10ms，Recall@10 > 0.95

#### 4. 混合检索
- [ ] 标量预过滤：先用 Bloom Filter / metadata 索引过滤，再做向量搜索
- [ ] 范围查询 + 向量：`WHERE timestamp > xxx AND vector_similarity(query, top_k=10)`
- [ ] 分数融合：支持标量相关性 + 向量相似度的加权排序

#### 5. 索引与 Compaction 协调
- [ ] Compaction 时增量重建索引（只处理变化的部分）
- [ ] 多版本索引管理：Compaction 期间查询使用旧索引，完成后原子切换
- [ ] 删除处理：tombstone 在索引中标记为不可见

#### 6. 性能优化
- [ ] SIMD 加速距离计算（AVX2 / AVX-512）
- [ ] 批量查询优化：一次查询多个向量，复用索引遍历
- [ ] 预取优化：减少随机访问导致的 cache miss

### 可观测性（必须实现）

- [ ] 索引统计：节点数、边数、层数分布
- [ ] 查询指标：QPS、P50/P99 延迟、Recall
- [ ] 插入指标：吞吐、索引构建耗时
- [ ] 存储指标：SSTable 大小、索引文件大小、内存占用

### Benchmark（必须实现）

- [ ] 写入 Benchmark：顺序插入 100 万向量，测吞吐
- [ ] 查询 Benchmark：随机查询 1000 次，测延迟分布和 Recall
- [ ] 混合 Benchmark：50% 写 + 50% 读
- [ ] 对比基线：纯暴力搜索（验证 Recall）、纯内存 HNSW（验证开销）

### 延后/不做的功能（简历可提但不实现）

- [ ] 动态维度（当前只支持固定维度）
- [ ] 分布式部署（单机版足够）
- [ ] GPU 加速（CPU 版本已经够用）
- [ ] 量化压缩（PQ/OPQ，工程量大）
- [ ] 多种索引类型（IVF、NSW，HNSW 足够）

---

## 技术架构

### 数据格式

#### 1. 逻辑层

```cpp
// 向量记录
struct VectorRecord {
    std::string key;              // 用户自定义 key
    std::vector<float> vector;    // 向量数据
    std::string metadata;         // JSON 格式的元数据
};

// sequence 与 tombstone 由现有 LSM/MVCC 存储层管理，不暴露在 VectorRecord 中。

// 查询请求
struct VectorQuery {
    std::vector<float> query_vector;
    int top_k;
    int ef_search;                // HNSW 参数
    
    // 可选的标量过滤
    std::function<bool(const std::string& metadata)> filter;
};

// 查询结果
struct VectorResult {
    std::string key;
    float distance;
    std::string metadata;
};
```

#### 2. 存储层

```
Key 编码格式：
  [user_key(var)] + [sequence(8 bytes)]

Value 编码格式：
  [magic="KVV1"(4)] + [element_type(1)] + [dim(4)]
  + [vector(dim*4)] + [metadata_len(4)] + [metadata(var)]

  element_type = 1: Float32
  VectorDelete 复用 LSM 原有 tombstone，不编码为 value
```

### HNSW 索引结构

```cpp
// HNSW 图节点
struct HNSWNode {
    uint64_t internal_id;         // 内部递增 ID
    std::string user_key;         // 用户 key（用于查找 vector）
    std::vector<float> vector;    // 向量副本（避免回表）
    
    // 每层的邻居列表
    std::vector<std::vector<uint64_t>> neighbors;  // neighbors[layer] = {id1, id2, ...}
};

// HNSW 索引
class HNSWIndex {
    int M_;                       // 每层最大边数
    int ef_construction_;         // 构建时搜索宽度
    int max_layer_;               // 最大层数
    float ml_;                    // 层数分布参数（通常 1/ln(M)）
    
    uint64_t entry_point_;        // 入口节点
    std::vector<HNSWNode> nodes_; // 所有节点
    
    // 核心接口
    void Insert(const std::string& key, const std::vector<float>& vec);
    std::vector<VectorResult> Search(const std::vector<float>& query, int k, int ef_search);
    void Remove(const std::string& key);  // 标记删除，不真正删除节点
};
```

### 索引持久化

```
索引文件格式：index_XXXXXX.hnsw

Header:
  [magic(4)] + [version(4)] + [M(4)] + [ef_construction(4)] + [max_layer(4)] + [num_nodes(8)]

NodeSection:
  For each node:
    [internal_id(8)] + [user_key_len(4)] + [user_key(var)] + [dim(4)] + [vector(dim*4)]
    + [num_layers(4)]
    + For each layer: [num_neighbors(4)] + [neighbor_ids(num_neighbors*8)]

Footer:
  [entry_point(8)] + [checksum(4)]
```

### LSM 与索引的集成

#### 1. 写入路径

```
Put(key, vector, metadata)
  |
  v
WriteBatch 编码 -> WAL append + fsync
  |
  v
MemTable 插入（key+seq -> encoded_value）
  |
  v
HNSW.Insert(key, vector)  // 增量更新内存索引
  |
  v
MemTable 达到阈值 -> Flush
  |
  v
生成 SSTable + 导出对应的 HNSW 子图
  |
  v
Manifest 记录新文件 + 索引文件路径
```

#### 2. 读取路径

```
Search(query_vector, top_k)
  |
  v
HNSW.Search() -> 候选 key 列表
  |
  v
对每个 key:
  MemTable 查找 -> 如果找到且可见，返回
  SSTable 查找 -> Bloom Filter -> Block Index -> DataBlock
  |
  v
按 distance 排序，返回 top_k
```

#### 3. Compaction 路径

```
选择待 Compact 的 SSTable 文件
  |
  v
流式归并所有 key-value
  |
  v
生成新的 SSTable
  |
  v
同时重建 HNSW 子图：
  - 只保留最新版本的向量
  - tombstone 不加入索引
  |
  v
持久化新的索引文件
  |
  v
原子切换：更新全局索引引用
  |
  v
删除旧 SSTable 和索引文件
```

---

## 实现计划（6周）

### Week 1: 向量存储与编码

**目标**：支持向量数据的存储和检索，但还没有索引（纯暴力搜索）

**任务**：
- [x] 设计 `VectorRecord` 的编码/解码逻辑
- [x] 修改 `WriteBatch` 支持向量类型
- [x] 实现 `DB::PutVector(key, vector, metadata)`
- [x] 实现 `DB::GetVector(key) -> VectorRecord`
- [x] 实现暴力搜索 `DB::BruteForceSearch(query, top_k)`（作为 Recall 基线）
- [x] 单元测试：插入、查询、删除、重启恢复

**验收标准**：
- [x] 能插入 1000 个 768 维向量
- [x] 暴力搜索能返回正确的 top-10
- [x] 重启后数据不丢失

---

### Week 2: HNSW 索引基础实现

**目标**：实现内存版 HNSW，支持插入和搜索

**任务**：
- [x] 阅读 HNSW 论文，理解算法细节
- [x] 实现 `HNSWIndex` 类框架
- [x] 实现层数选择逻辑（指数分布）
- [x] 实现插入算法：`SearchLayer` + `SelectNeighbors` + `Connect`
- [x] 实现搜索算法：从 entry_point 贪心下降 + beam search
- [x] 单元测试：小数据集（1000 向量）验证 Recall

**验收标准**：
- [x] 1000 个 128 维向量，Recall@10 > 0.90
- [x] 搜索延迟 < 1ms

验收由 `hnsw_test` 使用固定随机种子执行，并以暴力搜索结果计算 Recall。

**参考资料**：
- 论文：Malkov & Yashunin (2016) "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs"
- 参考实现：hnswlib（C++）

---

### Week 3: 索引与 LSM 集成

**目标**：将 HNSW 索引集成到 LSM 写入和读取路径

**任务**：
- [x] 修改 `MemTable::Add`，插入时同步更新内存 HNSW
- [x] 修改 `DB::Search`，使用 HNSW 代替暴力搜索
- [x] 处理 tombstone：删除时在索引中标记不可见
- [x] 处理 MVCC：Search 时过滤 `seq > snapshot_seq` 的记录
- [x] 集成测试：并发写入 + 查询，验证正确性

**验收标准**：
- 10 万向量，插入吞吐 > 1 万/s（benchmark 入口待补充）
- 查询 QPS > 5000，Recall@10 > 0.95（HNSW acceptance 基线）

---

### Week 4: 索引持久化与恢复

**目标**：索引能随 SSTable Flush 持久化，重启后恢复

**任务**：
- [ ] 设计索引文件格式（见上文）
- [ ] 实现 `HNSWIndex::Save(filename)` 和 `Load(filename)`
- [ ] 修改 Flush 逻辑：生成 SSTable 时同步生成索引文件
- [ ] 修改 Manifest：记录索引文件路径
- [ ] 修改恢复逻辑：重启时加载所有索引文件，合并成全局索引
- [ ] 测试：插入 -> Flush -> 重启 -> 验证查询结果一致

**验收标准**：
- 重启后查询结果与重启前完全一致
- 索引文件大小合理（< 向量数据的 2 倍）

---

### Week 5: Compaction 与索引重建

**目标**：Compaction 时增量重建索引，保持查询可用

**任务**：
- [ ] 设计增量索引重建策略：
  - 方案 A：重建整个索引（简单但慢）
  - 方案 B：只更新变化的节点（复杂但快）→ 先实现方案 A
- [ ] 修改 Compaction 逻辑：归并 SSTable 时同步重建索引
- [ ] 实现索引版本管理：Compaction 期间查询使用旧索引
- [ ] 实现原子切换：Compaction 完成后切换到新索引
- [ ] 测试：大量写入 -> 触发 Compaction -> 验证查询不中断

**验收标准**：
- Compaction 期间查询不返回错误
- Compaction 后 Recall 不下降
- 旧版本向量被正确清理

---

### Week 6: 性能优化与 Benchmark

**目标**：优化性能，达到可展示的指标

**任务**：
- [ ] SIMD 优化距离计算（AVX2）
- [ ] 批量查询优化（一次查询多个向量）
- [ ] 内存布局优化（减少 cache miss）
- [ ] 实现完整 Benchmark 套件：
  - 写入 Benchmark：100 万向量，测吞吐
  - 查询 Benchmark：1000 次随机查询，测延迟和 Recall
  - 混合 Benchmark：50% 写 + 50% 读
- [ ] 生成 Benchmark 报告（见下文模板）

**验收标准**（目标指标）：
- **写入**：吞吐 > 5 万/s（768 维）
- **查询**：QPS > 1 万，P99 < 10ms，Recall@10 > 0.95
- **混合**：写入吞吐 > 2 万/s，查询 QPS > 5000

---

## Benchmark 报告模板

```markdown
# 向量数据库 Benchmark 报告

## 测试环境
- CPU: [型号]
- 内存: [大小]
- 磁盘: [SSD/HDD]
- 编译器: GCC 11.2, -O3
- 向量维度: 768

## 数据集
- SIFT1M（100 万 128 维向量）或自生成数据
- 查询集：1000 个随机向量

## 写入性能
| 指标 | 结果 |
|------|------|
| 总向量数 | 1,000,000 |
| 总耗时 | XX 秒 |
| 吞吐 | XX 万/s |
| P99 延迟 | XX ms |

## 查询性能
| 指标 | 结果 |
|------|------|
| QPS | XX 万 |
| P50 延迟 | XX ms |
| P99 延迟 | XX ms |
| Recall@10 | 0.XX |
| Recall@100 | 0.XX |

## 混合负载（50% 写 + 50% 读）
| 指标 | 结果 |
|------|------|
| 写入吞吐 | XX 万/s |
| 查询 QPS | XX 万 |
| 查询 P99 | XX ms |

## 存储开销
| 指标 | 结果 |
|------|------|
| SSTable 大小 | XX MB |
| 索引文件大小 | XX MB |
| 内存占用 | XX MB |

## 对比分析
| 系统 | QPS | P99 | Recall@10 |
|------|-----|-----|-----------|
| 本项目 | XX | XX ms | 0.XX |
| 纯暴力搜索 | XX | XX ms | 1.00 |
| 纯内存 HNSW | XX | XX ms | 0.XX |
```

---

## 技术难点与解决方案

### 难点 1: 索引与 Compaction 的一致性

**问题**：Compaction 会删除旧版本 SSTable，但索引可能还在使用

**解决方案**：
- 索引使用引用计数或版本号管理
- Compaction 生成新索引后，原子切换全局索引引用
- 旧索引等待所有查询完成后再删除

### 难点 2: HNSW 插入性能

**问题**：HNSW 插入是 O(log N)，但常数较大，高并发时可能成为瓶颈

**解决方案**：
- 批量插入优化：积累一批向量后，并行插入索引
- 锁粒度优化：每层单独加锁，减少锁竞争
- 延迟索引构建：MemTable 可以先不建索引，Flush 时再统一建

### 难点 3: 距离计算开销

**问题**：768 维向量的欧氏距离计算较慢

**解决方案**：
- SIMD 加速（AVX2 一次处理 8 个 float）
- 缓存对齐：向量按 32 字节对齐，减少 cache miss
- 预计算：query 向量归一化后，内积等价于余弦相似度

### 难点 4: 混合检索实现

**问题**：标量过滤 + 向量搜索如何高效组合

**解决方案**：
- 策略 1：先用标量索引（B-Tree/Hash）过滤，再在子集上向量搜索
- 策略 2：向量搜索返回候选，再用标量条件过滤（适合过滤率低的场景）
- 当前版本：先实现策略 2（简单），后续优化为策略 1

---

## API 设计

```cpp
class VectorDB {
public:
    struct Options {
        std::string db_path;
        int vector_dim;              // 固定维度
        int hnsw_M = 16;             // HNSW 参数
        int hnsw_ef_construction = 200;
        
        // 继承现有 LSM 参数
        size_t memtable_size = 4 * 1024 * 1024;
        int max_level = 2;
    };
    
    // 打开/关闭
    Status Open(const Options& options);
    Status Close();
    
    // 写入
    Status PutVector(const std::string& key, 
                     const std::vector<float>& vector,
                     const std::string& metadata = "");
    Status DeleteVector(const std::string& key);
    Status WriteBatch(const VectorWriteBatch& batch);
    
    // 查询
    struct SearchOptions {
        int top_k = 10;
        int ef_search = 50;          // HNSW 参数
        const Snapshot* snapshot = nullptr;
        std::function<bool(const std::string&)> metadata_filter;
    };
    
    std::vector<VectorResult> Search(
        const std::vector<float>& query,
        const SearchOptions& options);
    
    // 混合查询示例
    auto results = db.Search(query_vec, {
        .top_k = 20,
        .ef_search = 100,
        .metadata_filter = [](const std::string& meta) {
            // 解析 JSON，过滤 category == "tech"
            return ParseJSON(meta)["category"] == "tech";
        }
    });
    
    // 管理
    Status Compact();  // 手动触发 Compaction
    std::string Stats();  // 返回统计信息
    
    // Snapshot（继承现有能力）
    const Snapshot* GetSnapshot();
    void ReleaseSnapshot(const Snapshot* snapshot);
};
```

---

## 简历描述模板

### 项目名称
**基于 LSM-Tree 的轻量级向量数据库**

### 技术栈
C++17 / LSM-Tree / HNSW / SIMD / Protobuf / CMake

### 项目描述
在现有 LSM-KV 存储引擎基础上实现向量存储和近似最近邻（ANN）搜索，支持高吞吐写入、混合检索和崩溃恢复。适用于语义搜索、推荐系统和 RAG 应用。

### 核心功能
- 实现 HNSW（Hierarchical Navigable Small World）索引，支持高效的 ANN 搜索
- 设计向量与标量数据的混合存储格式，支持 Compaction 时的增量索引重建
- 实现查询优化：Bloom Filter 预过滤、SIMD 加速距离计算、批量查询复用
- 支持 MVCC Snapshot 隔离和 tombstone 可见性控制
- 完整的 WAL + Manifest 崩溃恢复机制

### 性能指标
- 写入：100 万 768 维向量，吞吐 XX 万/s
- 查询：QPS XX 万，P99 延迟 < 10ms，Recall@10 > 0.95
- 存储：索引文件大小 < 向量数据 2 倍，内存占用 XX MB

### 技术难点
1. **索引一致性**：Compaction 期间保持查询可用，通过多版本索引管理和原子切换解决
2. **并发控制**：分离 write / memtable / version / background 四把锁，读取 I/O 不占用全局锁
3. **性能优化**：AVX2 SIMD 加速距离计算，向量内存对齐减少 cache miss
4. **混合检索**：支持标量过滤 + 向量相似度的组合查询，实现多条件排序

---

## 需要补充的知识点

### 1. HNSW 算法（必须掌握）
- 论文精读：Malkov & Yashunin (2016)
- 理解层次结构、贪心路由、邻居选择策略
- 参考实现：hnswlib

### 2. 向量检索基础
- 距离度量：欧氏距离、内积、余弦相似度
- Recall 与 Precision 的定义
- ANN 与精确搜索的 tradeoff

### 3. SIMD 编程
- AVX2 intrinsics 基础
- 向量化距离计算
- 内存对齐要求

### 4. Benchmark 方法
- 标准数据集：SIFT1M、GIST1M
- Recall 计算方法
- 延迟分位数统计

---

## 风险与应对

### 风险 1：时间不够

**应对**：
- 优先级排序：Week 1-4 是 MVP，Week 5-6 是加分项
- 最坏情况：只做到 Week 4，索引能持久化即可
- Compaction 可以简化：全量重建索引（不做增量）

### 风险 2：性能达不到目标

**应对**：
- 降低数据规模：10 万向量也够展示
- 简化场景：只做固定维度（128 维更快）
- 突出正确性：Recall 比 QPS 更重要

### 风险 3：HNSW 算法理解不透

**应对**：
- 先实现简化版：固定层数（2 层）、固定 M（16）
- 参考 hnswlib 源码，边看边写
- 单元测试：小数据集（100 向量）先跑通

### 风险 4：面试被问倒

**应对**：
- 准备标准问题清单（见下文）
- 对每个模块都能深入 2-3 层
- 诚实：不懂的地方说"这块我还没优化，但我知道可以..."

---

## 面试问题准备（预测）

### 存储层
Q: 向量数据如何编码？为什么这样设计？
A: [type(1) + dim(4) + vector(dim*4) + metadata]，定长头 + 变长尾，方便解码和跳过

Q: Compaction 时如何处理向量？
A: 流式归并，只保留最新版本，tombstone 不写入新 SSTable

Q: 向量数据量很大，如何优化存储空间？
A: 可以用量化（PQ）或压缩（Zstd），当前版本未实现但保留了扩展点

### 索引层
Q: 为什么选 HNSW 而不是 IVF 或 Annoy？
A: HNSW 查询性能最好（对数复杂度 + 小常数），构建稍慢但可增量更新

Q: HNSW 的 M 和 ef_construction 如何调优？
A: M 越大召回越高但内存越大（一般 16-64），ef_construction 越大构建越慢但质量越好（一般 100-400）

Q: 插入时如何选择层数？
A: 指数分布，概率 = 1/M，生成 0 到 max_layer 之间的随机层数

Q: 删除向量后，索引中的节点怎么办？
A: 标记为 deleted，查询时跳过，Compaction 时真正删除

### 系统层
Q: 并发插入时，索引如何保证一致性？
A: 每层单独加读写锁，插入只锁涉及的层，查询用读锁

Q: Compaction 期间查询会中断吗？
A: 不会，Compaction 生成新索引，完成后原子切换，旧索引引用计数为 0 时删除

Q: 如果索引文件损坏怎么办？
A: 索引有 checksum，加载时校验，失败则从 SSTable 重建

Q: 内存占用如何控制？
A: 索引常驻内存（当前设计），如果太大可以 mmap 或只缓存热点层

### 算法层
Q: 如何计算 Recall？
A: 暴力搜索得到真实 top-K，与 HNSW 结果求交集，交集大小 / K

Q: 为什么不用精确搜索？
A: O(N) 太慢，100 万向量查一次要几百毫秒，ANN 可以达到 10ms 以下

Q: 如何处理高维诅咒？
A: HNSW 通过分层 + 贪心路由缓解，但维度太高（> 1024）效果会下降，可以用降维（PCA）

Q: 混合检索如何实现？
A: 先 HNSW 搜索返回候选，再用标量条件过滤，最后按分数排序

---

## 后续扩展方向（简历可提）

以下是项目完成后可以继续优化的方向，**简历上可以写"后续计划"**，面试时体现思考深度：

1. **量化压缩**：实现 Product Quantization（PQ），将 768 维压缩到 64 字节，减少内存占用
2. **多种距离度量**：支持 Hamming 距离（二值向量）、Jaccard 相似度（集合）
3. **分布式版本**：分片存储，路由层选择分片，每个分片独立索引
4. **GPU 加速**：距离计算和 top-K 选择用 CUDA，适合超大批量查询
5. **在线学习**：支持向量的增量更新（用户反馈调整 embedding）

---

## 总结

这个改造方案的核心优势：
1. **技术深度够**：LSM + HNSW 两个经典算法
2. **独立可验证**：不需要外部依赖，Benchmark 说话
3. **简历亮点清晰**：每一行都能深挖 2-3 层
4. **实现周期可控**：6 周能做出 MVP，4 周也有 fallback

最重要的是，这个项目能让你在面试时掌握**话语权**：
- 面试官问 LSM，你比他更懂（因为你写过）
- 面试官问 HNSW，你能讲清楚原理和实现
- 面试官问性能，你有 Benchmark 数据

祝顺利！有任何问题随时问我。
