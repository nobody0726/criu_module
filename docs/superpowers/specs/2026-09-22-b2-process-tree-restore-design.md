# B2 进程树 Restore 设计合同

**状态：** 草案，待用户审阅  
**日期：** 2026-09-22  
**前置：** B1 核心 gate 已完成；A7 已提供进程树 dump 侧数据模型  
**验证环境：** Lima `criu-dev` 构建，Linux 5.10.29/aarch64 嵌套 QEMU guest 验证

## 1. 目标

B2 将 B1 的“单进程、单线程、独立地址空间 restore”扩展为“多进程树 restore”。
本阶段的重点不是增加新的地址空间搬迁机制，而是正确处理进程树恢复中的结构性顺序、
session/pgid 关系、跨进程同步和失败清理。

B2 必须能够从真实 CRIU 生成的 `pstree.img` 和每个任务的 B1 支持范围镜像中恢复：

- 多个独立进程；
- 每个进程仍为单线程；
- 每个进程拥有独立 `mm_struct`；
- 精确恢复被纳入 restore closure 的 PID；
- 通过正确的父进程创建子进程，恢复 `ppid`；
- 恢复 session leader 和 session 继承关系；
- 恢复 pgid，并处理组长尚未完成初始化的等待；
- 每个进程复用 B1 的 VMA、TLS、sigframe、bootstrap 和 `rt_sigreturn` 路径；
- 任一可诊断失败都能超时退出并清理已创建的目标 PID。

## 2. 明确不在 B2 核心范围内

以下能力不计入 B2 首个核心 gate：

- 多线程和 `CLONE_VM`；
- 共享 `files_struct`、共享 fd table；
- pipe、UNIX socket、TCP socket 和 `SCM_RIGHTS`；
- `MAP_SHARED`、shmem、memfd、POSIX/SysV shm；
- PID/mount/network/user namespace；
- cgroup、mount、cwd/root 和其他 fs 上下文；
- `TASK_HELPER`；
- session leader 已经退出的复杂拓扑；
- 根任务原始 `ppid` 的精确恢复；
- vDSO 跨地址 relocation；
- dirty file-private/COW、timer、credentials、seccomp 和 LSM；
- 完整 ZDTM restore 矩阵。

这些项目在 B2 计划末尾登记为 B 轨后续待办。它们可以依赖 B2 的树恢复基础设施，
但不能改变 B2 核心 gate 的判定。

## 3. 设计原则

### 3.1 结构性关系必须在创建时正确

Linux 没有 `setppid()`，也没有加入任意 session 的系统调用。因此：

- 谁创建子进程决定 `ppid`；
- 子进程从创建者继承 session；
- `setsid()` 必须在需要它的子进程创建前完成；
- `clone3(set_tid)` 必须在创建阶段指定目标 PID；
- `CLONE_VM`、`CLONE_FILES` 等共享关系不能事后补上。

B2 采用构造式恢复，不在创建后修改这些不可逆关系。

### 3.2 可补救关系使用等待和断言

pgid 可以通过 `setpgid()` 在组长存在后恢复，因此不改变树的构造方式。每个需要
加入其他进程组的任务等待组长通过共享 futex 标记“已建立组”，并带超时。

正确的 session 继承关系不重复执行系统调用，而是通过 `getsid()` 断言；只有 session
leader 执行 `setsid()`。

### 3.3 所有可失败操作先于最终地址空间提交

每个任务必须在 B1 `COMMIT` 前完成本范围内所有可失败动作：

- 镜像解析和验证；
- backing file 打开；
- fd、signal mask、rlimit 等支持的 B 类状态；
- session/pgid 设置；
- staging VMA 和页面填充；
- B1 `VALIDATE`；
- TLS、sigframe 和 bootstrap 准备。

只有所有任务都进入“可提交”状态后，才能进入全局 commit 前屏障。B1 `COMMIT` 和
`rt_sigreturn` 属于单向阶段，不能依赖它们之后的用户空间回滚。

## 4. 总体架构

```text
真实 CRIU 镜像
    │
    ▼
用户空间 pstree reader / topology validator
    │
    ├─ 建立 rst_item 树
    ├─ 检查单线程、独立 mm、支持的 PID namespace
    ├─ 在第一次 clone 前建立 MAP_SHARED scratch
    ├─ 建立全体目标 PID registry
    │
    ▼
递归 restore coordinator
    │
    ├─ 第一遍 fork：需要在 setsid 前出生的孩子
    ├─ session leader 执行 setsid()
    ├─ 第二遍 fork：其余孩子
    ├─ pgid leader/futex 等待
    ├─ 每个节点执行 B1 per-task restore
    ├─ 全局 commit 前屏障
    │
    ▼
B1 kernel-assisted restore
    │
    ├─ VALIDATE
    ├─ COMMIT
    └─ AArch64 bootstrap -> rt_sigreturn
```

B2 不复制 B1 的 VMA 搬迁逻辑。每个任务拥有自己的 B1 restore context，树层只负责
拓扑和同步。

## 5. 数据模型

### 5.1 `rst_item`

`rst_item` 是用户空间的拓扑蓝图，每个进程在 fork 后拥有自己的私有副本：

```c
struct rst_item {
	pid_t			pid;
	pid_t			ppid;
	pid_t			pgid;
	pid_t			sid;
	pid_t			born_sid;
	struct rst_item		*parent;
	struct list_head	children;
	struct list_head	siblings;
	struct rst_ctx		*ctx;
	unsigned		shared_idx;
};
```

`parent`、`children` 和 `siblings` 只用于每个任务自己的 restore 控制流。跨进程状态
不能通过这些指针访问，因为 fork 后它们位于不同地址空间中的私有副本。

### 5.2 `rst_shared`

`rst_shared` 必须在第一次 `clone3()` 之前以 `MAP_SHARED | MAP_ANONYMOUS` 建立：

```c
struct rst_shared {
	unsigned	total_tasks;
	unsigned	ready_count;
	unsigned	commit_go;
	unsigned	abort;
	unsigned	pgrp_set[];
	unsigned	task_state[];
	pid_t		pids[];
};
```

它只承载 restore 施工阶段需要的共享控制信息：

- 全局任务数量；
- B 类准备完成计数；
- commit release/abort 状态；
- 每个 pgid leader 的就绪标志；
- 每个已创建 PID 的状态；
- 超时和失败传播所需的最小错误信息。

`rst_shared` 不承载 protobuf、不承载页面数据，也不假设 B1 `COMMIT` 后仍然存在。
由于 B1 会替换当前地址空间，所有依赖 scratch 的屏障必须发生在 `COMMIT` 之前。

### 5.3 PID registry

主协调者从解析出的 `pstree.img` 建立全体目标 PID registry。每个成功创建的任务都
立即登记：

```text
target pid -> rst_item -> creation state -> precommit state -> terminal state
```

清理路径只依赖 registry 逐个处理 PID，不依赖当前 session 或 pgid，因为这些关系
可能已经在失败前发生变化。

## 6. Restore 顺序

### 6.1 前置解析和验证

主进程读取 `pstree.img` 和每个节点对应的 B1 镜像，检查：

- 所有 `ppid` 都能在 closure 中找到，根节点除外；
- 每个 PID 唯一，且目标 PID namespace 与当前 B2 支持范围一致；
- 每个节点只有一个线程；
- 不存在 `CLONE_VM`、共享 fd table 或共享资源引用；
- session leader 和所需 pgid leader 都在 closure 中；
	- 从 `sid`、父链和 session leader 推导 `born_sid`，并检查推导过程中没有冲突；
- session leader 已退出、TASK_HELPER 或无法构造的拓扑明确返回
  `-EOPNOTSUPP`；
- 每个节点的 B1 镜像都能在不可逆阶段前完成验证。

### 6.2 建立 scratch 和根任务

1. 解析全部镜像；
2. 建立 `rst_shared`；
3. 初始化 PID registry；
4. 打开并完成根任务的 B1 transaction；
5. 用 `clone3(set_tid)` 创建根任务；
6. 根任务进入递归 `rst_create_children_and_session()`；
7. 主进程保留 root PID、所有 transaction fd 和清理状态。

根任务的原始 `ppid` 不在 closure 时无法精确恢复。B2 支持
`--restore-sibling` 等价模式，让根任务成为调用者的 sibling；但文档和 gate 必须
明确这不是恢复原始外部父进程。

### 6.3 两趟 fork 和 session

每个任务按照 CRIU `create_children_and_session()` 的语义递归执行：

1. 第一遍 fork 所有必须在当前 session 中出生的孩子；
2. 如果当前任务是 session leader，执行 `setsid()`；
3. 断言当前 session 与目标 `sid` 一致；
4. 第二遍 fork 其余孩子；
5. 子任务立即进入自己的递归流程，不返回父任务的 sibling 循环。

真实 CRIU `pstree.proto` 不携带 `born_sid` 字段。B2 在读完全部节点后，复用 CRIU
`prepare_pstree_ids()` 的语义推导它：对每个 `sid != pid` 且父节点 session 不同的
任务，从父节点沿祖先链向上走到对应 session leader；路径上的每个祖先都记录该
子任务的 session 作为 `born_sid`。同一个祖先如果被推导出两个不同的
`born_sid`，必须拒绝镜像。不能通过“先全部 fork、再 setsid()”简化，因为这会永久
破坏混合 session 拓扑。

### 6.4 pgid 恢复

所有需要加入其他进程组的任务：

1. 找到目标 pgid 对应的 leader；
2. 在共享 scratch 中等待 leader 的 `pgrp_set`；
3. 以有限超时执行 `setpgid(0, target_pgid)`；
4. 如果自己是 leader，设置并唤醒对应 futex；
5. 用 `getpgid(0)` 断言最终结果。

组长失败时，等待者必须收到超时/abort，而不是永久阻塞。

### 6.5 每个任务的 B1 restore

拓扑完成后，每个任务独立执行 B1 流程：

1. 准备本任务的 fd、signal mask、rlimit 等支持状态；
2. 建立 staging VMA 并填充页面；
3. 准备 TLS、sigframe 和 bootstrap；
4. 提交本任务的 B1 `VALIDATE`；
5. 将自身状态标记为 `READY_TO_COMMIT`。

所有任务都达到 `READY_TO_COMMIT` 后，才允许通过全局 commit 前屏障。

### 6.6 commit 前屏障和最终跳转

commit 前屏障包含两个条件：

- `ready_count == total_tasks`；
- `abort == 0`。

满足条件后由协调者设置 `commit_go` 并唤醒所有任务。每个任务随后：

1. 发起 B1 `COMMIT`；
2. 立即跳转到 B1 bootstrap；
3. 设置 `TPIDR_EL0`；
4. 执行 `rt_sigreturn`；
5. 从原始 PC 继续执行。

`COMMIT` 之后不再进行用户空间屏障，因为旧 restore 地址空间已被替换。任何
commit 后异常都按“整体 restore 失败”处理，由主协调者根据 PID registry 逐个清理。

## 7. 错误处理与清理

### 7.1 可逆阶段

在第一次 `COMMIT` 之前，任何任务失败都必须：

- 设置共享 `abort`；
- 唤醒所有 futex 等待者；
- 关闭未提交 transaction fd；
- 由主协调者逐个终止已经创建的目标 PID；
- `waitpid()` 或等价方式确认可等待的直接子进程退出；
- 对无法直接 wait 的后代通过记录的 PID 检查 `/proc/$pid` 已消失；
- 清空 PID registry 和临时 staging 资源。

### 7.2 不可逆阶段

进入任一任务的 `COMMIT` 后，不能声称具有事务回滚能力。实现必须通过以下策略
降低风险：

- 所有任务先完成 B1 `VALIDATE`；
- 所有可检测失败在 commit 前屏障之前报告；
- commit 后某个任务异常时，主协调者停止其余已恢复任务；
- 逐个 PID 清理，不使用单一进程组作为唯一清理手段；
- gate 只有在全体任务存活、拓扑正确且 dmesg 干净时才输出 PASS。

### 7.3 超时

以下等待必须有统一超时：

- pgid leader 就绪；
- 全局 ready barrier；
- commit release；
- 主进程等待 root 完成；
- 清理阶段等待 PID 消失。

超时错误必须带上 task PID、目标 pgid/session、当前阶段和已登记任务数，便于诊断
死锁发生在哪个阶段。

## 8. 文件和接口边界

计划中的主要文件：

```text
userspace/mini-restore/rst_pstree.c
userspace/mini-restore/rst_fork.c
userspace/mini-restore/rst_session.c
userspace/mini-restore/rst_shared.c
userspace/mini-restore/rst_cleanup.c
tests/progs/tree-session.c
tests/b2-restore.sh
```

需要修改：

```text
userspace/mini-restore/main.c
userspace/mini-restore/Makefile
userspace/mini-restore/carrier.c
userspace/mini-restore/restore.h
docs/steps/B2-pstree-restore.md
```

B2 不修改 B1 内核 restore ABI 的语义。若需要扩展 transaction metadata，必须保持
版本化字段、`VALIDATE` 深拷贝和 `COMMIT` 不读取用户指针的约束。

## 9. 验证门禁

### 9.1 核心用例

- 平坦树：一个根任务和三个子任务；
- 五层链式树；
- 混合 session 拓扑；
- 多个 pgid，组长和成员创建顺序不同；
- 每个任务的内存 marker、tick 和 TLS；
- 根任务 `--restore-sibling` 模式；
- PID 已占用；
- session leader 已退出，明确 `-EOPNOTSUPP`；
- barrier 前单个任务失败，其他任务不永久挂起；
- B1 单进程回归；
- 中等规模树（至少 50 个任务）的超时和清理。

pipe/socket 连通性不属于 B2 核心用例，必须留到共享 fd/socket restore 扩展任务。

### 9.2 权威环境

所有内核相关 gate 必须通过：

```text
macOS：编辑、Git、编排
Lima criu-dev：Linux/ARM64 构建
Linux 5.10.29/aarch64 QEMU guest：加载模块、真实 CRIU dump、B2 restore
```

镜像、日志和 restore 工作目录必须使用 guest-local `/tmp`，不能把 Lima 9p 共享目录
当作 restore 工作目录。

### 9.3 PASS 条件

B2 gate 只有在以下条件全部满足时输出：

```text
B2_PROCESS_TREE_RESTORE: PASS
```

- 所有目标 PID 创建成功；
- 每个任务的 `ppid`、pgid、sid 与支持范围内的目标值一致；
- 根任务的外部 ppid 限制被明确标记，而不是伪造为原值；
- 每个任务的 B1 地址空间和执行现场恢复成功；
- 所有 marker/tick/TLS 检查通过；
- 失败路径不会留下目标 PID；
- guest `dmesg` 无 Oops、BUG、WARNING、KASAN、refcount 或 use-after-free。

## 10. B 轨后续待办

本节是 B2 设计和实现计划末尾的 backlog。条目只有在具备独立设计、实现和验证
证据后才能从待办移出，不得因 B2 核心 gate 通过而自动视为完成。

| 编号 | 后续任务 | 前置/依赖 | 当前状态 |
|---|---|---|---|
| B2-E1 | TASK_HELPER、session leader 已退出拓扑 | B2 核心树模型 | 待设计 |
| B2-E2 | 根任务外部 ppid 的更完整 sibling/parent 语义 | 调用者模型、pid namespace | 已知限制 |
| B2-E3 | 多线程、`CLONE_THREAD`、`CLONE_VM` | A4、B2 核心屏障 | 待设计 |
| B2-E4 | 共享 fd table、pipe、UNIX/TCP socket | A5/A8、soccr 语义 | 待设计 |
| B2-E5 | shmem、memfd、POSIX/SysV shm | A8、namespace 语义 | 待设计 |
| B2-E6 | PID/mount/network/user namespace | A9、PID namespace | 待设计 |
| B2-E7 | cgroup、mount、cwd/root 和 fs context | A9、权限顺序 | 待设计 |
| B2-E8 | dirty file-private/COW、vDSO relocation | B1 VMA 扩展 | 待设计 |
| B2-E9 | timer、credentials、seccomp、LSM | A6、A9 | 待设计 |
| B2-E10 | target PID occupied、duplicate COMMIT live-kernel 测试 | Linux guest 扩展矩阵 | 待验证 |
| B2-E11 | 完整 ZDTM restore allowlist 和跨场景矩阵 | B2 核心及全部资源扩展 | 待规划 |

## 11. CRIU 参照实现

B2 的流程直接参考仓库中的 CRIU 实现：

- `criu/criu/cr-restore.c:create_children_and_session()`
- `criu/criu/cr-restore.c:fork_with_pid()`
- `criu/criu/cr-restore.c:restore_sid()`
- `criu/criu/cr-restore.c:restore_pgid()`
- `criu/criu/include/restorer.h` 的 restore stage/futex 组织
- `criu/criu/pie/restorer.c:restore_thread_common()`
- `criu/criu/pie/restorer.c:unregister_libc_rseq()`

本项目保留 B1 的“用户空间解析 + 内核辅助 VMA 提交 + 用户空间
`rt_sigreturn`”分工，不把 CRIU 的 protobuf 解析或完整资源恢复逻辑复制进内核。
