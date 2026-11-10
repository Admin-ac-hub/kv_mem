# 固定三节点 KV 集群

这是新增的三节点模式；原有 `kv_server` / `kv_client` 仍是独立的单节点模式，**不会自动变成集群**。
三节点模式使用 `kv_cluster_server` / `kv_cluster_client`，每个节点独占自己的数据目录。

## 能做到什么

- 成功写入意味着日志已在至少两个节点落盘，并且 Leader 已将整批操作应用到本地 KV。
- 一台停止或断电，另外两台自动选主，切换期间请求可能失败或写入结果未知；选出新主后可继续读写。
- 落后的节点恢复后自动补日志；所需历史已压缩时，先分块安装快照，再补齐后缀日志。客户端读写只由 Leader 完成；读取也先通过一次多数派日志屏障。
- 只剩一台、无法联系多数派时，不返回成功读写。三台一起停机不能提供服务；磁盘完整的节点恢复多数后可重选主。
- 超时不是回滚。写入发出后未获得可信结果，返回 `Unknown`（CLI 退出码 3），客户端不会自动重发。

这不是“任何断电/磁盘故障都不丢数据”的保证。安全依赖本地文件系统和磁盘正确执行 `fsync`、原子 `rename` 和目录同步。
不覆盖磁盘丢失、两份以上副本同时损坏、硬件谎报落盘、恶意节点。三进程同机只能演示进程故障；要容忍整机断电，应放到三台独立主机及独立持久盘上。

## 构建

需要 Linux（miniRPC 使用 epoll/eventfd）；复用 [原有 RPC 构建命令](MINIRPC_INTEGRATION.md#构建和运行)，开启 `KV_ENABLE_RPC=ON` 后同时生成集群程序。

macOS 当前环境可以先执行：

```bash
./scripts/run_rpc_checks.sh
```

脚本会将代码复制到已有的 `minirpc-linux` 容器，构建并运行 core、单节点 RPC 和全部集群测试。
构建路径每次不同，以脚本输出为准；后续命令在 Linux 或该容器内执行。

## 启动三个节点

配置文件样例为 `rpc/cluster.example.conf`：

```text
local-three-node
1 127.0.0.1:19101 127.0.0.1:19201
2 127.0.0.1:19102 127.0.0.1:19202
3 127.0.0.1:19103 127.0.0.1:19203
```

第一行为集群 ID，后面依次为 `节点 ID 业务地址 节点间地址`；三份配置必须完全相同。
仅支持显式 IPv4 和端口，不使用 DNS。若分开部署，换成三台主机实际可互通的地址。

先创建数据父目录，然后在三个终端分别运行（`build-rpc` 换成实际构建目录）：

```bash
mkdir -p ./cluster-data
./build-rpc/rpc/kv_cluster_server rpc/cluster.example.conf 1 ./cluster-data/node1
./build-rpc/rpc/kv_cluster_server rpc/cluster.example.conf 2 ./cluster-data/node2
./build-rpc/rpc/kv_cluster_server rpc/cluster.example.conf 3 ./cluster-data/node3
```

以上三个 server 命令各占用一个终端，不要在同一前台串行执行。一般约一两秒选出 Leader；这不是硬实时切换保证。

如果使用脚本创建的容器构建，命令形如：

```bash
# 用本轮脚本打印的路径替换这个示例路径；在容器内使用该目录下的 rpc/cluster.example.conf。
docker exec -it minirpc-linux /tmp/kv-rpc-check.EXAMPLE/build/rpc/kv_cluster_server \
  /tmp/kv-rpc-check.EXAMPLE/rpc/cluster.example.conf 1 /tmp/kv-rpc-check.EXAMPLE/node1
```

另外两个终端将 ID/数据目录分别换为 `2/node2` 和 `3/node3`。容器 `/tmp` 仅用于开发验收；生产数据必须放到持久卷，不能依赖临时目录或容器可写层。

## 客户端

```bash
./build-rpc/rpc/kv_cluster_client rpc/cluster.example.conf status
./build-rpc/rpc/kv_cluster_client rpc/cluster.example.conf put hello world
./build-rpc/rpc/kv_cluster_client rpc/cluster.example.conf get hello
./build-rpc/rpc/kv_cluster_client rpc/cluster.example.conf write put a 1 put b 2 delete hello
```

客户端查询三个地址发现 Leader，不需要手工修改主节点地址。`status` 是诊断信息，不是多数派读证明，隔离的旧主可能仍自报 Leader；真实读写必须通过多数派屏障。
业务批次沿用 4096 操作、1 MiB/value、1.5 MiB 编码请求上限。C++ Client 保留二进制 value；CLI 无法表示 NUL。

退出码：0 成功，1 已知错误/暂不可用，2 参数错误，3 写入结果未知。未知写入可按业务读回核实，但后续覆盖/删除可能使读回无法确定历史结果；目前没有请求去重或历史结果查询，不能盲目重试。

## 故障与恢复

- 正常停止：发送 SIGINT/SIGTERM，先停止共识工作并唤醒等待请求，再 drain 两个 RPC 服务，最后关闭 DB。
- 异常停止：可以在开发环境强杀其中一个 server。保留目录，用原命令、原 ID、原配置重新启动；自动追赶。
- 配置和身份绑定到持久状态；不能直接改节点 ID、地址、集群 ID，也不能把旧单机目录直接当集群数据目录。
- 每个目录的 `raft.state` 是带 CRC 的任期、投票、提交位置、快照元数据和日志后缀；`raft-INDEX-TERM.snapshot` 保存该位置的有序 KV 数据。`data/` 是可重建的物化副本。重启时清理并重建 `data/`，恢复所引用快照，再顺序重放已提交后缀；未提交日志不能直接应用。
- 持久化或 KV 应用失败后，该节点进入 fail-stop，不继续投票或提供成功读写。先排查磁盘，再用原目录重启。
- 缺失/损坏 `raft.state`、其引用的快照缺失/损坏、身份不匹配会拒绝启动，**不要删除状态文件强行启动**。本版未提供副本更换或丢盘重建流程；应先停止操作、备份故障盘并人工核实健康多数派，不能把这种操作当普通节点重启。
- 不要删除运行中节点的 LOCK，也不要绕过集群程序直接写 `data/`。

## 容量与部署边界

固定三个成员；没有动态扩缩容、分片、远程用户 Iterator/Snapshot、TLS、鉴权或 exactly-once。仅在受信网络使用；节点间端口必须限制为三台成员互通，业务端口也需外部访问控制。

Raft 自动在保留日志达到 **8 MiB 或 1024 条**且有已应用前缀时生成快照；读取追加的屏障也计入条数。
快照流式保存实际存活 KV，独立于 64 MiB 日志文件保护上限；多数派正常提交时，累计历史超过 64 MiB 后可持续压缩运行。
64 MiB 仍保护未提交日志的异常积累；失去多数派不能压缩未提交记录，也不能承诺继续成功写入。
快照大小受实际数据量、可用内存/磁盘与本地引擎限制，没有新增 64 MiB 快照大小限制。

持久化仍重写当前保留的日志后缀；快照生成/恢复在应用锁内执行，大数据集或慢盘可能增加延迟、超时及选举扰动。
本轮解除累计历史容量上限，未作吞吐或大数据集停顿时间承诺。设计、恢复顺序和验收见 [Raft 快照](RAFT_SNAPSHOTS.md)。

升级请先停止全部三个节点、备份各自完整目录，再统一替换二进制并用原配置/目录启动。
新版本读取旧版 v1 完整日志；首次快照后使用 v2 状态，旧版会拒绝打开。不要混用版本滚动升级或只回滚二进制。

## 有限测试集

```bash
# 已构建的 Linux 目录；运行全部带 raft 标签的测试。
./scripts/run_cluster_checks.sh build-rpc
```

- `raft_test`：投票持久化、只有两份日志且存活副本尚不知已提交时的选主恢复、提交先于 KV 应用的重放恢复、未知写入不自动重放、落后候选者拒绝、目录独占、身份/CRC 拒绝、写入/fsync/rename/目录同步失败、未提交日志冲突修复、已提交前缀保护、活旧主双向消息隔离及合并恢复。
- `raft_snapshot_test`：快照生成/发布/截日志/清理的崩溃和 I/O 边界，安装中断、CRC/顺序校验、匹配后缀保留/冲突后缀丢弃，空快照、最大 key/value、纯屏障日志压缩。
- `cluster_snapshot_process_test`：真实 TCP 累计确认写入 72 MiB，强杀正在安装快照的落后副本并恢复，后缀复制、全体强杀重启与逐副本数据核验。
- `cluster_process_test`：三个独立进程真实 TCP，批次/删除/空值/二进制，强杀 Leader 后选主和读写，节点重启追赶，暂停旧主后重新加入，仅剩单节点拒绝成功读写，全体强杀重启后保留已确认数据。

进程强杀仍保留操作系统缓存，不等于真实机器断电。消息隔离是可控的进程内传输故障，不等于物理网络设备测试。本轮不做无限随机测试、性能矩阵或硬件断电测试。

## 2026-11-09 第一版验收记录（快照前）

环境：现有 `minirpc-linux` 容器，Linux aarch64，GCC 13.3.0，Protobuf 3.21.12。源码来自当前工作区，无自动提交或推送。

| 检查 | 本轮最终结果 |
| --- | --- |
| miniRPC 原有 Release CTest 基线 | 20/20 通过 |
| KV + 单节点 RPC + 三节点 Debug 全量 CTest | 11/11 通过（31.09 秒） |
| ASan + UBSan：两个新增安全/进程测试 | 2/2 通过（19.38 秒） |
| TSan：新增共识/恢复/故障测试 | 1/1 通过（6.75 秒） |
| 集群 CLI Put/Get/Write | 包含在三进程测试内，通过 |

上述 sanitizer 配置插桩 KV、适配层和新集群代码；导入的 miniRPC 库及系统 Protobuf 未重新插桩。未观察到 sanitizer 报告；仅代表已执行路径。

独立代码审查未报告已确认的生产逻辑缺陷；指出的三个定向验收缺口已补齐：仅两份持久日志且存活副本未知 commit、commit 已落盘但 KV 落后、Unknown 写入不重试。
Debug 测试最初暴露“NotFound 被误当成换主重试”的服务层错误，已修复，并在最终回归中验证删除后读取和全重启不复活。

本轮 Linux 构建保留在容器 `/tmp/kv-rpc-check.Z6N8iO/build`，ASan/UBSan 和 TSan 分别为同级 `build-asan` / `build-tsan`。测试子进程结束后均已停止，未留下常驻集群。临时路径不是长期部署目录。

## 2026-11-10 收口与 CI

新增 `.github/workflows/ci.yml` 的 Linux RPC cluster Debug/Release 任务，安装 Protobuf、构建并安装固定提交
`Admin-ac-hub/miniRPC@35ab857c3bf2284524d287704a5f8ce599e3647a`，开启 `KV_ENABLE_RPC=ON`，运行全量 CTest。
依赖使用已提交源码，不依赖相邻 miniRPC 工作区的未提交变更。

最初本地在 `minirpc-linux`（Linux aarch64 / GCC 13.3 / Protobuf 3.21.12）使用本地已提交但尚未推送的 miniRPC `a33956a` 干净源码构建，
Debug 全量 **11/11 通过，31.15 秒**。构建目录为 `/tmp/kv-raft-20261110/build`。
GitHub Actions 任务配置已加入；远端执行结果需推送后确认，本地结果不代表远端 CI 已通过。

远端依赖可获取性检查发现 `a33956a` 尚未推送，CI 已改为远端 main 的固定提交 `35ab857`。
集群传输显式设置 Protobuf 帧编码，不依赖后来本地提交改变的默认值；公开依赖复验结果另记。

使用 `affb095` 的独立干净副本，仅补显式帧编码、链接公开依赖 `35ab857` 后，
Linux Debug 全量 **11/11 通过，30.22 秒**；目录 `/tmp/kv-raft-20261110/checkpoint/build`。

## 2026-11-10 快照里程碑

已加入持久快照、自动日志压缩与分块安装，替换第一版的累计历史上限。
Debug / Release 全量均 13/13，ASan+UBSan 集群 4/4，TSan 定向 2/2；
真实 TCP 累计确认 75,498,651 字节并完成安装中断恢复与全体重启。详见 [完整验收及原始日志](RAFT_SNAPSHOTS.md#验证结果)。
