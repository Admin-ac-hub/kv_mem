# Raft 快照、日志压缩与快照安装

2026-11-10。基于固定三节点 Raft；运行方式见 [集群文档](THREE_NODE_CLUSTER.md)。

## 数据契约

- Raft 逻辑索引从 `snapshot_index + 1` 开始映射日志数组；快照边界本身保留 `snapshot_term`，用于选举、日志匹配与冲突检查。
- 应用锁串行保护 DB 写入、applied index 与快照扫描。仅保存已经完整应用的前缀，扫描开始至结束不允许后续批次写入；不把本地 DB sequence 当作 Raft index。
- `raft.state` 保存任期/投票/commit、快照 index/term/bytes 及其后缀，保留原先长度、CRC、临时文件原子发布协议。旧 v1 状态可读，首次快照升级至 v2；旧版拒绝 v2。
- `raft-INDEX-TERM.snapshot` 为独立不可变文件。32 字节头包含格式、index/term 和头 CRC；随后是长度+CRC 的有序 Put 批次，最后以独立结束标记确认完整性。空数据库也有完整快照。
- 快照只存活键值，覆盖后的历史与墓碑不进入快照。二进制/空 value 保留；有序唯一 key、key/value 限制、记录长度、CRC、结束标记和总长度在恢复时校验。每条记录有界，文件总大小不受 64 MiB 日志保护上限限制。

## 发布与恢复顺序

1. 锁内确定 `S = applied_index`、`TermAt(S)`，按当前 DB 视图流式写 `snapshot.tmp`。
2. 同步快照文件，rename 到独立的 index/term 文件名，再同步节点目录。
3. 此时才删除内存日志的 `<= S` 前缀，更新快照元数据；将新的 `raft.state` 写临时文件、fsync、rename、fsync 目录。
4. 第 3 步可靠完成后，才删除旧快照并同步目录。持久化或应用失败立即 fail-stop。

崩溃发生于第 3 步发布前，旧 `raft.state` 仍能引用旧快照和完整后缀；未引用的新文件不参与恢复。
第 3 步发布后，新状态引用的快照已经可靠落盘；旧文件删除中断最多留下多余副本。
`raft.state` 的原子 rename 中断后允许旧/新版本任一被保留，两者各自都有完整恢复链。

每次启动都将 `data/` 当作物化缓存重建：验证权威 Raft 文件，重建空 DB，恢复被引用快照，再按顺序应用 `(snapshot_index, commit]`。
不依赖另存 applied marker，也不在旧 DB 上叠加快照；安装删除后的键不会因旧物化数据复活。
重建途中再次崩溃，下一次仍从权威文件重建。被引用文件缺失、CRC/格式/身份错误拒绝启动，绝不静默清空为新节点。

## 自动压缩和远端安装

- 每次完成已提交日志应用后，保留日志达到 8 MiB 或 1024 条即快照；屏障/no-op 也计数，避免只读运行的历史无限增长。未提交后缀始终保留。
- Leader 的 `next_index` 落到快照边界内时，改发 `Install`。每次最多 256 KiB，包含集群身份、发送任期、快照 index/term、总长度、offset 和块 CRC；沿用独立节点间 RPC 资源与有界单次交换。
- Follower 仅按连续 offset 写 `install.tmp`。中间块确认表示暂存进度；进程重启后从 offset 0 重传。完整文件同步、发布并全量校验后才发布 Raft 元数据和 commit，再重建 DB，最后确认 installed index。
- 最终确认丢失后重发可返回已安装位置；`index <= commit` 的旧快照不会回滚应用状态。乱序/坏块拒绝，实际持久化故障 fail-stop。
- 本地在快照 index/term 有匹配条目时保留后缀，否则丢弃冲突尾部；完成安装后恢复普通 AppendEntries。投票的新旧比较、当前任期多数派提交和读取屏障继续使用逻辑索引。

## 验收范围

`raft_snapshot_test` 对快照生成、文件 fsync/rename/目录同步、Raft 状态写入/fsync/rename/目录同步、旧快照清理注入 I/O 错误及子进程直接退出。
安装测试覆盖暂存、发布、状态切换、DB 重建中的子进程退出，并验证中断后恢复或从零重传。
每次恢复检查覆盖值、删除、二进制/空值以及未提交数据不可见；另覆盖匹配后缀保留、冲突后缀丢弃、坏快照拒绝、空快照、最大 key/value 和 1024 条纯屏障自动压缩。

`cluster_snapshot_process_test` 使用三个独立进程和真实 TCP。一个副本停机，另外两台累计确认 **75,498,651 字节（超过 72 MiB）**的业务日志，持续生成快照并检查保留日志小于 16 MiB。
让落后副本开始安装后强杀并重启，验证重传完成、后缀继续复制，再全体强杀/重启，逐副本核验最终值、删除、空/二进制/大 value。
该容量用例反复覆盖同一 key，验证的是累计历史超过旧上限；不是 72 MiB 存活数据集的性能测试。

进程退出和 SIGKILL 保留 OS 缓存，不能替代真实机器断电。断电恢复安全依赖文件系统/磁盘的 fsync、原子 rename 和目录同步契约；未做硬件断电或长期随机压力测试。
快照扫描/恢复持有应用锁，延迟随存活数据和磁盘速度增长；目前没有异步快照或增量检查点。

## 验证结果

环境：`minirpc-linux`，Linux aarch64，GCC 13.3.0，Protobuf 3.21.12；本地容器构建根目录 `/tmp/kv-raft-20261110`。

| 检查 | 结果 | 原始记录 |
| --- | --- | --- |
| Debug，core + RPC + 全部 Raft | 13/13，82.44 秒 | [debug.txt](validation/20261110-raft-snapshots/debug.txt) |
| Release，core + RPC + 全部 Raft | 13/13，43.81 秒 | [release.txt](validation/20261110-raft-snapshots/release.txt) |
| ASan + UBSan，全部 4 项 Raft | 4/4，128.09 秒 | [asan-ubsan.txt](validation/20261110-raft-snapshots/asan-ubsan.txt) |
| TSan，共识/隔离恢复与 TCP 快照追赶 | 2/2，56.37 秒 | [tsan.txt](validation/20261110-raft-snapshots/tsan.txt) |

[Debug 定向输出](validation/20261110-raft-snapshots/debug-details.txt) 保留各故障边界和累计字节数；
[TSan 定向输出](validation/20261110-raft-snapshots/tsan-details.txt) 保留共识和 TCP 容量恢复结果。
Sanitizer 插桩 KV、适配层和 Raft；导入的 miniRPC 静态库及系统 Protobuf 未重新插桩。已执行路径未观察到报告，不代表覆盖依赖库全部内部逻辑。

CI 的 Linux Debug/Release 开启 `KV_ENABLE_RPC=ON`，包括以上两项新测试；GitHub Actions 远端执行待推送后确认。
依赖固定为公开的 `Admin-ac-hub/miniRPC@35ab857c3bf2284524d287704a5f8ce599e3647a`。
