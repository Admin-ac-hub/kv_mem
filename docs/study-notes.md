# KV 存储引擎学习笔记

## 1. internal_iterator.h — 内部迭代器接口

纯虚基类，定义了遍历 KV 存储引擎的协议。

| 方法 | 作用 |
|---|---|
| `SeekToFirst()` | 定位到最小的 key |
| `Seek(target)` | 定位到 >= target 的第一个 key |
| `Next()` | 前进到下一个 key |
| `Valid()` | 当前位置是否有效 |
| `key()` | 返回当前 key |
| `value()` | 返回当前 value |
| `sequence()` | 返回当前 entry 的序列号（MVCC 用） |
| `deleted()` | 当前 entry 是否是 tombstone（删除标记） |
| `status()` | 返回操作状态 |

典型用法：
```cpp
for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!it->deleted()) {
        std::cout << it->key() << " => " << it->value() << std::endl;
    }
}
```

---

## 2. status.h — 错误状态封装

工厂方法模式，用 static 方法构造不同类型的错误：

```cpp
Status::OK()
Status::NotFound("key not found")
Status::IOError("failed to open file")
Status::Corruption("checksum mismatch")
Status::InvalidArgument("bad param")
```

好处：语义清晰、防止误用、集中管理。

---

## 3. types.h — 基础类型定义

```cpp
using SequenceNumber = std::uint64_t;  // 版本号，单调递增

struct VersionedEntry {
    std::string key;
    SequenceNumber sequence = 0;
    std::string value;
    bool deleted = false;              // tombstone 标记
};
```

---

## 4. MVCC（多版本并发控制）

每次写操作带一个单调递增的 SequenceNumber，同一个 key 可以有多个版本：

```
key="name", seq=100, value="alice"
key="name", seq=95,  value="bob"
key="name", seq=90,  deleted=true   ← tombstone
```

读操作拿到一个 read_seq（比如 97），只能看到 seq <= 97 的版本。

作用：
1. 快照读：读写并发不冲突
2. 删除用 tombstone：真正的物理删除推迟到 compaction
3. Iterator 的 `sequence()` 和 `deleted()` 让上层过滤版本

---

## 5. Tombstone（删除标记）

LSM-Tree 中删除不直接删，而是写一条 `deleted=true` 的记录。

原因：SSTable 是只读的，没法回去改旧文件。

点查会先查较新的 MemTable/SSTable，遇到最新可见 tombstone 就停止并返回未找到；范围迭代器则归并各层版本，再按 sequence 过滤 tombstone 和旧值。

物理删除：在 Compaction（压缩合并）时清理。

---

## 6. LSM-Tree 架构

```
写入路径:
  Write → [WAL日志] → [MemTable (内存)] → 满了 → flush → [SSTable_L0 (磁盘)]
                                                          ↓ compaction
                                                       [SSTable_L1, L2, ...]

读取路径:
  Read → MemTable → SSTable_L0 → SSTable_L1 → ... (从新到旧找)
```

| 层 | 位置 | 特点 |
|---|---|---|
| WAL | 磁盘日志 | 崩溃恢复用，顺序写 |
| MemTable | 内存 | 跳表，支持有序插入和查找 |
| SSTable | 磁盘 | 排好序的不可变文件，分层 |

InternalIterator 是 LSM-Tree 的统一遍历协议。DB 为 MemTable 和 SSTable 创建对应的 child iterator，再由 MergingIterator 做多路归并。

---

## 7. std::optional（C++17）

表示一个值可能存在也可能不存在。

```cpp
std::optional<Entry> result = Get(key);
if (result.has_value()) {    // 或直接 if (result)
    std::cout << result->value;  // 用 -> 解引用
} else {
    std::cout << "not found";
}
```

比返回空指针或特殊值更安全、语义更清晰。

---

## 8. Rule of Five（五法则）

如果类管理了资源，需要手动控制 5 个特殊成员函数：

```cpp
~T();                      // 析构
T(const T&);               // 拷贝构造
T& operator=(const T&);    // 拷贝赋值
T(T&&);                    // 移动构造
T& operator=(T&&);         // 移动赋值
```

规则：手写其中一个，大概率需要手写全部。

`= delete` 禁止编译器生成某个函数，调用时编译报错。

SkipList 禁止了拷贝（跳表里有大量堆内存，拷贝代价大且无意义）。

---

## 9. 跳表（Skip List）

有序链表 + 多层索引，O(log n) 查找，实现比红黑树简单。

```
Level 2:  head -----------------------> 50 ---------> NULL
Level 1:  head ---------> 20 ---------> 50 ---------> NULL
Level 0:  head -> 10 -> 15 -> 20 -> 30 -> 40 -> 50 -> 70 -> NULL
```

查找从最高层开始，跳过了就往前走，跳不过就往下一层。

插入时用 `RandomLevel()` 决定节点层数（50% 概率晋升），靠概率自动保持平衡。

对比红黑树：实现简单，底层链表天然支持范围遍历。当前 SkipList 本身不负责同步，并发安全由 DB 外层的 `memtable_mu_` 保证。

---

## 10. skiplist.cc 详解

### Internal Key 编码

```
user_key + '\0' + inverted_sequence(8字节大端)
```

`inverted = max - sequence`，所以 sequence 越大排序越靠前（最新版本在前）。

### FindGreaterOrEqual — 核心查找

从最高层往下找，每层往右走直到 next >= target，记录每层前驱到 `prev` 数组。

### prev 数组

**临时局部变量**，记录在每一层"新节点应该插在谁的后面"。

```
prev[i] = 在第 i 层，最后一个 < target 的节点
```

插入时：
```cpp
node->next[i] = prev[i]->next[i];   // 新节点接上前驱的后继
prev[i]->next[i] = node;            // 前驱接到新节点
```

就是链表插入，跳表有好多层所以需要数组记录每层前驱。

### Put — 插入

1. FindGreaterOrEqual 找位置，填充 prev
2. 如果 exact match，原地更新
3. RandomLevel 决定层数
4. 用 prev 在每层插入新节点

### Delete — 删除

和 Put 类似，但写入的 entry 标记 `deleted=true`（tombstone）。

### Get — 查找

用 `(key, read_sequence)` 编码后找。因为 sequence 反转编码，FindGreaterOrEqual 会跳过所有 sequence > read_sequence 的版本，直接找到 <= read_sequence 的最新版本。

---

## 11. memtable.h/.cc — 内存写缓冲

薄封装，所有操作转发给 SkipList：

```cpp
void MemTable::Put(...)    { table_.Put(...); }
void MemTable::Delete(...) { table_.Delete(...); }
auto MemTable::Get(...)    { return table_.Get(...); }
```

---

## 12. sstable.h/.cc — 磁盘有序表（部分）

### 文件布局

```
┌──────────────────┐
│  Data Block 0..N │  ← 实际数据
├──────────────────┤
│  Index Block     │  ← 每个 block 的位置信息
├──────────────────┤
│  Bloom Filter    │  ← 快速判断 key 是否存在
├──────────────────┤
│  Footer (40B)    │  ← index/filter 的偏移量
└──────────────────┘
```

### 前缀压缩（Prefix Compression）

相邻 key 有共同前缀，只存不同的部分：

```
shared = 和前一个 key 相同的前缀长度
unshared = 自己独有的部分
```

每 16 条设一个 Restart Point（存完整 key），用于随机访问时先二分跳到最近的 restart point 再顺序解码。

### SSTableInternalIterator

实现了 InternalIterator 接口，按 block 遍历 SSTable 中的数据。`SkipUntilVisible` 跳过 sequence > read_sequence 的不可见条目。

## 13. bloom_filter.h/.cc — 布隆过滤器

### 用途

快速判断 key 是否**可能**在 SSTable 中，避免无效磁盘 IO。

### 核心原理

- bit 数组 + 多个哈希函数
- 插入：把 key 对应的 k 个 bit 位置点亮（设为 1）
- 查询：检查那 k 个 bit 是不是全亮
  - 有灭的 → 一定不存在（无假阴性）
  - 全亮 → 可能存在（有假阳性，可控）

### 关键参数

- `bit_count_` = max(64, key数量 × bits_per_key)
- `hash_count_` = clamp(1, bits_per_key × 0.69, 30)
- `bits_per_key = 10` 时，误判率约 1%

### 双重哈希技巧

用 2 个哈希模拟 k 个：`bit = (h1 + i * h2) % bit_count`

### 文件格式

```
[bit_count_ 4B] [hash_count_ 4B] [bit数组 NB]
```

### 在 SSTable 中的位置

每个 SSTable 文件末尾自带一个，读取时先加载到内存，查询前先 MayContain 过滤。
