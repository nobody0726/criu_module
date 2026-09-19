# A7 进程树、Session 与进程组设计

**状态：** 设计已批准，实施计划待生成

**目标：** 在 Linux 5.10.29/aarch64 nested QEMU guest 中，把 A2 的单线程组冻结扩展为同一 PID namespace 内的完整 descendant closure，由内核采集稳定的进程树、session 和 process group 拓扑，由 userspace converter 生成多进程 CRIU image set，并用真实 `criu restore` 验证恢复后的关系、存活和行为。

**适用范围：** A7 首个 gate 只支持同一 PID namespace 内、无 TASK_HELPER 需求、session leader 和 process-group leader 均在 dump closure 中的普通多进程树。A7 是 dump 侧能力；两遍 fork、`setsid()`、`setpgid()` 等 restore 施工仍由后续 B2 负责。

**相关文档：**

- [A7 步骤说明](../steps/A7-pstree.md)
- [PID、会话与进程组原理](../principles/06-pid-and-session.md)
- [restore 顺序原理](../principles/09-restore-ordering.md)
- [A3 问题与解决方法复盘](../A3-问题与解决方法复盘.md)
- [A2 freeze 设计](2026-09-06-a2-freeze-design.md)
- [A4 threads 实施计划](2026-09-15-a4-threads-implementation.md)
- [A5/A6 扩展 backlog](2026-09-18-a5-a6-extension-backlog.md)
- CRIU `images/pstree.proto`、`criu/criu/pstree.c`、`criu/criu/cr-restore.c`

## 1. 设计原则

### 1.1 一个冻结闭包，一份权威集合

A7 不会在 A2 冻结完成后临时重新扫描进程树，再让 A3-A6 各自重新枚举任务。整个 dump transaction 使用一个 immutable frozen closure：

```text
target leader
    |
    +-- process set: every descendant process leader
    |       |
    |       +-- task set: every thread in that process
    |
    +-- one cgroup-v2 freezer operation
    +-- one generation and one namespace binding
```

`pstree`、TASK/THREAD、memory、FD、signal/timer 和最终 revalidation 必须引用同一集合。任一 collector 发现成员集合、父子关系、namespace 或共享对象状态变化，都整体 abort；不得生成缺节点的部分镜像。

### 1.2 结构性关系优先于恢复时补丁

Linux 没有把任意进程加入已有 session 的系统调用。`setsid()` 只能创建以调用者 PID 为 ID 的新 session；process group 可以通过 `setpgid()` 加入，但 group leader 必须存在。因此 dump 必须先证明：

- 每个进程的父节点在 closure 中，root 的父节点可以在 closure 外；
- 每个 `sid` 对应的 session leader 在 closure 中；
- 每个 `pgid` 对应的 process-group leader 在 closure 中；
- session 推导不存在歧义；
- A7 不需要临时 TASK_HELPER。

不能满足这些条件时明确返回 `UNSUPPORTED` 或 `INCONSISTENT`，而不是写入看似完整但无法恢复的 `pstree.img`。

### 1.3 环境固定

- macOS 只负责编辑和编排；
- Lima `criu-dev` 负责 ARM64 module、converter 和静态 fixture 构建；
- nested Linux 5.10.29/aarch64 QEMU guest 负责 `insmod`、dump、converter 和真实 restore；
- 不在 macOS 或 Lima 宿主内核中加载模块；
- A7 首个 gate 固定为同一 PID namespace，控制调用者与目标 namespace 必须一致。

## 2. A7 首个 gate 范围

### 2.1 支持

- 一个目标 process leader 及其全部 descendant processes；
- 每个 process 的完整 thread group；
- 当前 PID namespace 内的 `pid`、`ppid`、`pgid`、`sid`；
- 普通同 session 子树；
- 子进程调用 `setsid()` 创建独立 session 的场景；
- 子进程加入 closure 内 process group 的场景；
- root 的父进程在 closure 外，但以 external-root-parent 标记；
- userspace converter 生成标准 CRIU `pstree.img` 以及每进程/每线程 image。

### 2.2 明确不支持

- root 或 descendant 位于不同 PID namespace；
- 控制调用者和目标 active PID namespace 不同；
- closure 内 process leaders 位于多个原始 cgroup v2 路径；A7 首个 gate 只接受一个可由现有 cookie ABI 整体恢复的原始 cgroup；
- closure 外的 session leader 或 process-group leader；
- 需要 TASK_HELPER 才能构造的 session/pgid 拓扑；
- 跨进程 `CLONE_VM`（非 `CLONE_THREAD`）；
- 跨进程共享 `files_struct`、pipe、UNIX socket 或 SysV SHM；这些属于 A8；
- 僵尸进程的完整恢复语义；
- 跨 namespace 的 signal/timer target；
- 任意并发 fork 导致 closure 在冻结窗口内持续变化；
- B2 的 restore 施工实现；
- 全量 ZDTM 和 GitHub CI。

### 2.3 根任务与外部父进程

root 的原始 `ppid` 如果不在 dump closure 中，A7 记录 `ppid=0` 加 external-root 标志。CRIU restore 创建 root 时，它的父进程由 restore 环境决定，之后可能被 reparent 到 init 或 subreaper；Linux 没有把新进程重新挂回任意外部父进程的合法接口。

因此 A7 只承诺 closure 内的父子关系精确，明确不承诺 closure 外 root parent 的恢复。

## 3. 冻结闭包设计

### 3.1 两层集合

```c
struct criu_freeze_process_view {
	struct task_struct *leader; /* borrowed ref held by freeze context */
	struct task_struct *parent; /* borrowed while context is active */
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	bool root;
	bool external_parent;
	bool session_leader;
	bool process_group_leader;
	unsigned int task_count;
};

struct criu_freeze_task_view {
	struct task_struct *task; /* borrowed ref held by freeze context */
	pid_t tid;
	pid_t tgid;
	bool stopped;
};
```

实际实现可以把 `parent`、namespace 引用和 task 引用藏在 `freeze.c` 私有结构中；公开 accessor 只返回只读 view，不转移 ownership。任何 view 都不得跨 `criu_thaw()` 保存。

### 3.2 发现和稳定化流程

1. target 必须被规范化为 thread-group leader，并绑定 target generation。
2. 绑定 target 的 active PID namespace；若不等于控制调用者 namespace，返回 `-EOPNOTSUPP`。
3. 在 tasklist RCU/read-side 保护下，以显式队列或显式栈遍历 `children`，发现当前 namespace 内的 descendants。
4. 为每个 process leader 和其所有 threads 建立引用；不使用递归，避免深层树耗尽 16 KB kernel stack。
5. 调用新的内核 wrapper，把全部 process leaders 作为一个 process set 加入同一个临时 cgroup-v2 freezer。
6. freeze 后重复发现/核对 closure；一次完整扫描没有新增或退出成员，且全部 pinned task 均不再运行时才 settled。
7. 如果目标持续 fork、成员退出、generation 变化或达到超时，走统一 rollback，返回 `-EAGAIN` 或 `-ETIMEDOUT`，不打开 snapshot writer。
8. settled 后构造 immutable topology snapshot，先完成拓扑校验，再允许任何 `PSTREE` 或 process-scoped record 写盘。

冻结期间新 fork 的处理是有界的：A7 不承诺捕获无限增长的进程树。超过重试/时间预算属于一致性或超时失败，不得静默丢弃新子进程。

### 3.3 cgroup wrapper

现有 A2 wrapper 一次只冻结一个 thread group。A7 需要 Linux 5.10.29 patch 增加 process-set wrapper：

```c
int criu_cgroup_freeze_process_set(
	struct task_struct **leaders,
	unsigned int leader_count,
	struct criu_freezer_cookie **cookie,
	char *original_path, size_t original_len,
	char *temporary_path, size_t temporary_len);
```

wrapper 内部负责：

- 对所有 leader 建立 thread-group change 保护；
- 创建一个临时 child cgroup；
- 将全部 thread groups 加入同一个 child；
- 发起一次 freeze；
- 中途失败时反向恢复已移动成员、删除 child、释放引用；
- thaw 时整体解冻、恢复每个 process 的原始 cgroup、删除 child；失败时保留 cookie 供重试。

A7 首个 gate 要求 closure 内所有 process leaders 在冻结前属于同一个原始 cgroup v2 路径。发现多个原始路径时返回 `UNSUPPORTED`；跨 cgroup 的逐进程 cookie 和恢复顺序留给后续扩展，不在本 gate 中伪造成功。

模块不得直接调用 `cgroup_attach_task()`、`cgroup_freeze()` 或 cgroup 内部 helper；这些操作继续封装在 patched Linux 5.10.29 kernel core 中。

### 3.4 公开 accessor

```c
int criu_freeze_process_count(struct criu_freeze_ctx *, unsigned int *);
int criu_freeze_process_get(struct criu_freeze_ctx *, unsigned int,
				    struct criu_freeze_process_view *);
int criu_freeze_process_task_count(struct criu_freeze_ctx *,
					   unsigned int process_index,
					   unsigned int *);
int criu_freeze_process_task_get(struct criu_freeze_ctx *,
					 unsigned int process_index,
					 unsigned int task_index,
					 struct criu_freeze_task_view *);
```

旧 `include_children=false` 路径继续冻结一个 thread group，保持 A3-A6 legacy snapshot 行为。A7 通过 `include_children=true` 使用 process-set path。

## 4. Snapshot ABI

### 4.1 Header capability

保持顶层 snapshot version 为 1，新增：

```c
#define CRIU_SNAPSHOT_F_PSTREE (1U << 1)
```

`CRIU_SNAPSHOT_F_SIGNAL_TIMERS` 仍为 bit 0。设置 `CRIU_SNAPSHOT_F_PSTREE` 时，必须同时存在至少一个 `PSTREE` record，并且所有 process-scoped records 都带 owner prefix。旧 converter 遇到未知 header flag 必须拒绝；新 converter 继续兼容 flags 为 0 的 A3-A6 legacy snapshot。

### 4.2 PSTREE record

新增类型：

```c
CRIU_SNAPSHOT_REC_PSTREE = 19
```

固定 little-endian packed payload：

```c
struct criu_snapshot_pstree_record {
	uint32_t version;
	uint32_t flags;
	uint32_t pid;
	uint32_t tgid;
	uint32_t ppid;
	uint32_t pgid;
	uint32_t sid;
	int32_t  born_sid;
	uint32_t leader_pid;
	uint32_t thread_count;
	uint32_t namespace_scope;
	uint32_t reserved;
};
```

固定大小为 48 字节。字段语义：

- `pid`、`tgid`、`pgid`、`sid` 使用目标 PID namespace 的 virtual number；
- `pid == tgid == leader_pid` 必须成立；
- closure 内非 root 的 `ppid` 指向另一个 PSTREE node；root 使用 `ppid=0`；
- `born_sid=-1` 表示不需要额外的 pre-`setsid()` fork 约束；
- `namespace_scope=1` 表示当前 PID namespace；其他值首个 gate 拒绝；
- flags 至少包含 `ROOT`、`EXTERNAL_PARENT`、`SESSION_LEADER`、`PGRP_LEADER`。

### 4.3 Process-scoped TLV

新增 TLV flag：

```c
#define CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE (1U << 0)
```

设置该 flag 的 record payload 以 8 字节 owner prefix 开头：

```c
struct criu_snapshot_process_scope {
	uint32_t owner_pid;
	uint32_t reserved;
};
```

以下 record 在 A7 snapshot 中必须 process-scoped：

```text
TASK / REGS / CREDS / THREAD
MM / VMA / PAGE_RUN
FD / FS / PIPE_ENDPOINT / PIPE_DATA / UNIX_SOCKET / SOCKET_QUEUE
SIGACTION / SIGNAL_QUEUE / ITIMERS / POSIX_TIMERS
```

`PSTREE` 是全局 record，不带 owner prefix。legacy A3-A6 record 保持 TLV flags 为 0，不能把 A7 owner prefix 误解释为旧 payload 字段。

### 4.4 记录顺序

reader 不依赖顺序，但 kernel writer 使用确定顺序：

```text
global PSTREE
for each process sorted by pid:
    TASK / REGS / CREDS
    THREAD records sorted by tid
    MM / VMA / PAGE_RUN
    FD and object records
    SIGACTION / SIGNAL_QUEUE / ITIMERS / POSIX_TIMERS
REC_END
```

这只定义可复现输出顺序，不把顺序作为 owner 归属的隐式协议。

## 5. 拓扑和一致性验证

拓扑验证必须在打开 writer 或创建 output image directory 之前完成。

### 5.1 必须成立

- PSTREE PID 唯一；
- 恰好一个 root；
- 每个非 root `ppid` 存在且不会形成环；
- 每个 process 的 `tgid` 与 owner PID 一致；
- 每个 owner 的 THREAD 记录完整、TID 唯一、`tgid` 一致；
- 每个 `pgid` 的 leader（`pid == pgid`）在 closure 中；
- 每个 `sid` 的 leader（`pid == sid`）在 closure 中；
- `born_sid` 为 `-1` 或 closure 中存在的 session ID；
- session 推导不会要求同一个 parent 在两个互相冲突的 session 中出生；
- 所有 A6 `notify_tid` 指向 closure 内实际线程；
- 不存在 A8 范围的跨进程共享 `mm/files/object`。

### 5.2 born_sid 推导

Linux task_struct 不保存“子进程 fork 时父进程所在 session”。因此 A7 在 immutable topology 上按 CRIU 规则推导：当子节点的 `sid` 与父节点 `sid` 不同，且子节点本身不是 session leader 时，从该节点向父链标记必须在目标 session 变化前出生的祖先。若一个 ancestor 被要求同时拥有两个不同的 `born_sid`，返回 `INCONSISTENT`。

`born_sid` 是内部 A7 record 信息，不写进标准 `pstree.proto`；converter 在生成 CRIU image 前必须完成相同推导和冲突检查。

### 5.3 leader 缺失

以下状态直接 `UNSUPPORTED`：

- `sid` 对应 PID 不在 PSTREE 集合；
- `pgid` 对应 PID 不在 PSTREE 集合；
- session/pgid 需要 CRIU TASK_HELPER 才能重建；
- root 的外部 parent 被误当作必须恢复的内部节点。

## 6. Kernel collector 分层

新增文件：

```text
kernel_module/checkpoint/collect_tree.c
kernel_module/checkpoint/collect_tree.h
kernel_module/checkpoint/dump_pstree.c
kernel_module/checkpoint/dump_pstree.h
```

职责：

- `collect_tree`：发现 descendants、pin leaders/tasks、绑定 namespace、构建 parent/child topology、计算 born_sid、验证 A8 边界；
- `dump_pstree`：把已验证的 process view 序列化为 PSTREE records；
- `dump.c`：按 process set 调度已有 A3-A6 collectors，并为每个 record 设置 owner PID。

现有 collectors 的接口必须逐步从隐式 target 改为显式 process view；不得在 A7 path 中调用 `criu_target_get()` 或重新枚举 thread group。所有 file I/O 都发生在 tasklist/RCU 临界区之外。

## 7. Converter 与 CRIU image 映射

### 7.1 Model

converter 从单一 legacy model 扩展为按 PID 建索引的 process model：

```c
struct process_model {
	uint32_t pid;
	struct pstree_node pstree;
	struct blob_ref task;
	struct blob_list threads;
	struct blob_list vmas;
	struct blob_list fds;
	struct signal_timer_model signal_timers;
};

struct snapshot_model {
	struct pstree_table pstree;
	struct process_model *processes;
	size_t process_count;
};
```

解析阶段先建立 owner PID 索引，再做拓扑、thread、A6 target 和共享对象验证；所有验证通过后才生成 output directory。

### 7.2 CRIU 输出

对每个 process：

```text
pstree.img                    one global image
core-$pid.img                 process leader core
mm-$pid.img
fs-$pid.img
creds-$pid.img
core-$tid.img                 every non-leader thread
files.img / reg-files.img     closure-wide deduplicated objects
fdinfo-*.img
```

`pstree.img` 每个 `pstree_entry` 输出：

```protobuf
pid = node.pid
ppid = node.ppid
pgid = node.pgid
sid = node.sid
threads = node.thread_tids
```

`born_sid` 不新增到 CRIU protobuf；它只用于输出前的 CRIU-equivalent topology validation。若 CRIU image parser/restore 对当前拓扑仍要求 helper，A7 首个 gate 应拒绝，而不是尝试在 converter 中伪造 helper task。

### 7.3 对象归属

- memory、FS、creds、signals/timers 按 owner PID 生成；
- thread core 按 TID 生成，但必须能回溯到 owner process；
- FD object table 在整个 closure 上去重；
- 跨进程共享 object 不是 A7 的隐式成功条件，检测到 A8 场景即拒绝；
- 每个 process 的镜像缺失、重复或 owner 不匹配都使整个 output publish 失败。

## 8. 测试和验证矩阵

### 8.1 Host-side

新增：

```text
tests/a7-abi-contract.sh
tests/a7-tree-contract.sh
tests/a7-converter-images.sh
tests/a7-unsupported.sh
tests/fixtures/a7-snapshot-builder.py
```

正例至少包括：

- `tree-simple`：父子同 session、同 pgid；
- `tree-session`：子进程 `setsid()`，验证 born_sid 推导；
- `tree-pgid`：子进程加入 closure 内 process group。

负例至少包括：

- duplicate PID；
- orphan/nonexistent parent；
- missing session leader；
- missing process-group leader；
- born_sid conflict；
- duplicate/missing owner-scoped record；
- cross-PID-namespace marker；
- A8 cross-process `CLONE_VM`/`CLONE_FILES` marker；
- freeze fork race and timeout marker。

### 8.2 Nested guest

新增 `tests/a7-cross-restore.sh`，固定在 Lima `criu-dev` 中构建、在 Linux 5.10.29/aarch64 nested QEMU guest 中执行。每个 case 必须：

1. 启动 fixture 并等待 ready marker；
2. 设置 A7 root target 并执行 tree dump；
3. 确认 snapshot 原子发布、没有 `.tmp`；
4. converter 生成完整 image directory；
5. 真实 CRIU restore；
6. 检查所有预期 PID 仍存活；
7. 检查父子关系、session ID、process-group ID；
8. 触发 group/session 相关行为 marker；
9. 检查 guest `dmesg` 无 `BUG`、`Oops`、`WARNING`、atomic-sleep 或 locking diagnostic。

明确输出：

```text
A7_SIMPLE: PASS
A7_SESSION: PASS
A7_PGID: PASS
A7_CROSS_RESTORE: PASS
```

缺 CRIU、guest、内核或权限只输出 `SKIP: ENVIRONMENT`，不能冒充 PASS。

### 8.3 A3-A6 回归

A7 合并前后必须重新执行 A3-A6 已有 host/guest 门禁。旧 `include_children=false` path 的 snapshot ABI 和行为不得回归。

## 9. 失败分类和事务规则

| 情况 | 分类 | 镜像结果 |
|---|---|---|
| 缺 CRIU/QEMU/guest/root/目标内核 | `SKIP/ENVIRONMENT` | 不发布 |
| 跨 PID namespace、缺 leader、A8 共享对象 | `UNSUPPORTED` | 不发布 |
| 冻结期间 fork/exit 或 generation 改变 | `INCONSISTENT`/`TIMEOUT` | 不发布 |
| TLV、PID、owner、thread、born_sid 矛盾 | `FORMAT`/`INCONSISTENT` | 不发布 |
| module、converter、CRIU 真实错误 | `FAIL` | 不发布 |
| restore 返回 0 但 PID 消失、关系错误或 marker 失败 | `FAIL` | 只保留诊断产物 |

任何 collector 或 writer 失败都必须：

- abort snapshot temporary file；
- 不发布 `snapshot.bin`；
- thaw 整个 process set；
- 释放所有 leader/thread/namespace/cgroup 引用；
- 清理 converter staging directory；
- 保持后续 target 可重新选择。

## 10. 完成标准与后续工作

A7 首个 gate 完成的必要条件：

- A2 process-set freezer 和 immutable closure accessor 在 Linux 5.10.29 guest 可用；
- 所有 A3-A6 collectors 使用同一 closure；
- PSTREE 和 process-scoped TLV ABI 锁定；
- simple/session/pgid 三类 fixture 的 host-side 和 nested guest gate 通过；
- 缺 leader、跨 namespace、A8 sharing、fork race 明确拒绝且无残留；
- restore 后进程存活、父子关系、session/pgid 和行为均检查通过；
- A3-A6 原有门禁保持通过；
- verification 文档记录完整命令、guest kernel、CRIU identity 和 dmesg 结果。

以下不阻塞 A7 首个 gate：

- TASK_HELPER 自动生成；
- 跨 PID/user/mount/network namespace；
- 跨进程共享资源去重（A8）；
- 僵尸和外部 parent 的完整恢复；
- B2 restore 施工；
- 全量 ZDTM 与 GitHub CI。
