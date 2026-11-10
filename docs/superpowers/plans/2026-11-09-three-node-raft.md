# Fixed Three-node Raft Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Three fixed KV replicas retain acknowledged writes across one failed node and safely elect a replacement.

**Architecture:** Raft between dedicated peer RPC and existing KV. Persist a checksummed atomic state file before acknowledging votes/logs; replay committed batches on restart. Serialize business requests; use a replicated no-op as a read barrier.

**Tech Stack:** C++20, existing miniRPC frame/body codecs and servers, Protobuf, POSIX fsync/rename/flock/poll, existing KV.

---

### Task 1: Persistent protocol and storage
Files: `rpc/proto/raft.proto`, `rpc/include/kv_raft/{store,node,transport}.h`, `rpc/src/raft_store.cc`, `rpc/test/raft_test.cc`.
- [x] Add runnable failure-first checks for atomic recovery, corruption rejection, directory exclusion and injected write/fsync/rename errors.
- [x] Define persistent cluster identity, term, voted_for, commit index, ordered term/batch entries and Vote/Append messages.
- [x] Implement atomic checksum-protected persistence, strict validation and process directory lock. Check failed store operations never produce successful acknowledgements.

### Task 2: Consensus and service
Files: `rpc/src/raft_node.cc`, `rpc/src/raft_transport.cc`, `rpc/src/raft_service.cc`, `rpc/include/kv_raft/service.h`.
- [x] Write failure-first election/replication checks against three independent states.
- [x] Implement per-peer bounded nonblocking TCP calls using existing miniRPC codecs; never hold consensus mutex across network calls.
- [x] Implement one election timer plus independent peer workers. Persist term/vote before reply; use last-term/index voting, prev-index/term matching, and current-term majority commit.
- [x] Persist commit index before ordered KV application. Poison on any storage/application failure; replay committed history on startup.
- [x] Add bounded serialized proposals and read no-op barriers. Separate peer/business RPC servers. Return NotCommitted before admission and Unknown after ambiguous submission.

### Task 3: Executables and cluster client
Files: `rpc/apps/kv_cluster_{server,client}.cc`, `rpc/include/kv_raft/client.h`, `rpc/src/raft_client.cc`, `rpc/CMakeLists.txt`.
- [x] Add strict fixed-membership configuration parsing and durable identity validation.
- [x] Add leader discovery, redirect of explicitly rejected requests, and no automatic retries after unknown writes.
- [x] Add server lifecycle shutdown: stop admission, stop workers, drain handlers, close KV, release lock.

### Task 4: Bounded acceptance tests and documentation
Files: `rpc/test/cluster_process_test.cc`, `scripts/run_cluster_checks.sh`, `docs/THREE_NODE_CLUSTER.md`, `README.md`.
- [x] Run deterministic safety tests plus real TCP/process crash/restart tests once in Debug.
- [x] Run sanitizer tests on the new code; rerun only failures or changed paths, not a large benchmark matrix.
- [x] Run existing KV/RPC regression once, keep failure output and document exact results and excluded scenarios.
- [x] Document three-node startup, commands, failure recovery, disk/network safety limits and history-size ceiling.

No automatic commits or pushes. Existing user modifications remain untouched.

## 验收与实施说明

采用当前会话内顺序实施，在 `feat/three-node-raft` 分支保留工作区修改，不创建工作树、不提交。
最终 Debug 11/11、ASan/UBSan 2/2、TSan 1/1；详情见 `docs/THREE_NODE_CLUSTER.md`。
共识和客户端故障回归集中在 `rpc/test/raft_test.cc`，真实多进程测试在 `rpc/test/cluster_process_test.cc`。
用户原有文档/简历修改保持不动。未做真实硬件断电或多物理机部署，也未做性能压测。
