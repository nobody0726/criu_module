# A6 信号与定时器设计

**状态:** 设计已收敛；实施计划见 [A6 implementation plan](2026-09-18-a6-signals-timers-implementation.md)

**目标:** 在 Linux 5.10.29/aarch64 guest 中，由内核模块采集进程的 signal disposition、pending signal、interval timer 和 POSIX timer 状态，由 userspace converter 生成真实 CRIU core protobuf，并通过真实 `criu restore` 验证恢复后的行为。

**架构:** 内核只采集版本化 little-endian TLV 结构化数据，不在内核生成 protobuf。converter 先完整解析和校验 snapshot，再生成 CRIU core 镜像；任何不支持、缺失或不一致状态都必须整体失败，不能静默填充默认值。验证固定使用 Lima 构建和嵌套 Linux 5.10.29/aarch64 QEMU guest。

**相关规范:**

- [A6 步骤说明](../steps/A6-signals-timers.md)
- [A3 问题与解决方法复盘](../A3-问题与解决方法复盘.md)
- [A4 threads implementation](2026-09-15-a4-threads-implementation.md)
- [snapshot ABI](../../include/criu_snapshot.h)
- CRIU `images/core.proto`、`images/sa.proto`、`images/siginfo.proto`、`images/timer.proto`

---

## 1. 范围决议

### 1.1 A6 首个范围

- 每进程共享的完整 `sigaction` 表；
- 每线程 blocked mask；
- 每线程 private pending queue；
- 进程 shared pending queue；
- 原始 128 字节 `siginfo_t`，包括实时信号和 timer signal；
- `ITIMER_REAL`、`ITIMER_VIRTUAL`、`ITIMER_PROF`；
- POSIX timer 的 ID、clock、notify、signal、`sigev_value`、周期、剩余时间、目标 TID 和 overrun 采集；
- userspace converter 生成 CRIU core protobuf；
- Linux 5.10.29/aarch64 guest 中的真实 cross-restore 和行为验证。

### 1.2 明确不属于首个 gate 的项目

- `F_SETOWN/O_ASYNC` 的完整 fown 信号语义；
- `signalfd`、`timerfd` 等 fd 绑定状态；
- 跨进程、跨 PID namespace 的信号目标解析；
- 尚未定义 ABI 的特殊 timer/clock 语义；
- 精确恢复 `timer_getoverrun()` 的运行时数值；
- 全量 ZDTM 和 GitHub CI。

上述项目记录在 [A5/A6 后续扩展清单](2026-09-18-a5-a6-extension-backlog.md)，原则上 A9 完成后再实施。

### 1.3 overrun 决议

A6 首个 gate 必须：

- 从内核准确采集 `it_overrun`/`it_overrun_last` 对应的 CRIU overrun 字段；
- 在 snapshot ABI 和 CRIU protobuf 中保留该值；
- 验证镜像没有丢失该字段。

当前 stock CRIU 会读取 `posix_timer_entry.overrun`，但其 restorer 只创建并设置 timer，没有把 overrun 写回 Linux `k_itimer`。因此 A6 首个 gate 不宣称 restore 后 `timer_getoverrun()` 数值完全相同。精确运行时 overrun restore 是 A6 extension，除非未来另行加入 CRIU 或内核 restore-side 补丁。

---

## 2. 数据流与镜像边界

```text
A2 freeze settled
        |
        v
kernel siglock + timer it_lock snapshot
        |
        v
snapshot.bin.tmp -- atomic rename --> snapshot.bin
        |
        v
converter phase 1: parse + validate complete model
        |
        v
converter phase 2: build CRIU core protobuf in staging directory
        |
        v
atomic image directory publish
        |
        v
Linux 5.10.29/aarch64 guest: real criu restore
        |
        v
post-restore handler / pending / timer behavior checks
```

A6 使用 CRIU 现代的 core 内嵌字段，不新增旧式独立 `sigacts-*`、`signal-*`、`psignal-*` 或 timer 镜像：

| snapshot 数据 | CRIU 目标 |
|---|---|
| sigaction table | leader `task_core_entry.sigactions` |
| shared pending | leader `task_core_entry.signals_s` |
| private pending | 每个 `thread_core_entry.signals_p` |
| interval timers | `task_core_entry.timers.real/virt/prof` |
| POSIX timers | `task_core_entry.timers.posix[]` |

这样可以直接替换现有 converter 中的默认 sigaction、空 pending 和零 timer 占位逻辑，并保持 A3–A5 的镜像组织方式不变。

---

## 3. Snapshot ABI 扩展

### 3.1 版本和能力标志

当前顶层 snapshot version 保持为 1。A6 是新增 TLV 类型和能力标志，不改变已有 A3/A5 记录含义。

新增 header capability flag：

```c
CRIU_SNAPSHOT_F_SIGNAL_TIMERS
```

规则：

- 只有完整 A6 数据采集成功后才能设置该 flag；
- flag 存在时，四类 A6 record 必须全部存在；
- 旧 converter 遇到非零 header flag 或未知 mandatory record 必须拒绝；
- 新 converter 仍能读取没有该 flag 的 A3–A5 legacy snapshot；
- legacy snapshot 只能用于回归兼容，不可作为 A6 gate 通过证据。

每个新 record 有独立的 `record_version` 和 `entry_size`。旧 reader 对未知的 `< 0x8000` mandatory record 返回 unsupported，不能静默忽略。

### 3.2 新增 record 类型

建议使用连续类型编号：

```text
15  CRIU_SNAPSHOT_REC_SIGACTION
16  CRIU_SNAPSHOT_REC_SIGNAL_QUEUE
17  CRIU_SNAPSHOT_REC_ITIMERS
18  CRIU_SNAPSHOT_REC_POSIX_TIMERS
```

### 3.3 SIGACTION_TABLE

每进程一条，固定包含 64 个 signal entry。每项显式保存：

```text
u32 signo
u32 reserved
u64 handler
u64 flags
u64 restorer
u64 mask
u64 mask_extended
```

`mask_extended` 在 aarch64 为零，保留用于未来 ABI 扩展。converter 校验 1–64 编号连续且唯一；向 CRIU 输出时跳过 `SIGKILL` 和 `SIGSTOP`，生成 CRIU 所需的 62 项 `sa_entry`。

### 3.4 SIGNAL_QUEUE

每个队列至少一条记录，即使队列为空：

- shared queue：一条，`scope=SHARED`、`owner_tid=0`；
- private queue：每个冻结线程一条或多条，`scope=PRIVATE`、`owner_tid=tid`。

队列 header 保存：

```text
u32 record_version
u32 scope
u32 owner_tid
u32 total_count
u32 first_index
u32 entry_count
u32 entry_size
u32 siginfo_size       /* 必须为 128 */
u64 pending_mask
u64 reserved
```

每个 entry 保存：

```text
u32 signo
u32 reserved
u8  siginfo[128]
```

大队列可以拆成多个 TLV。converter 必须验证分片覆盖 `[0,total_count)`，无缺口、无重叠，且同一队列的 `pending_mask` 一致。

Linux 5.10.29 允许 pending bit 存在而没有 `sigqueue` 节点。此时内核按 `collect_signal()` 的规则生成合成 siginfo：`si_signo=signo`、`si_errno=0`、`si_code=SI_USER`、`si_pid=0`、`si_uid=0`。不能把这种状态静默丢弃或伪造成任意默认值。

### 3.5 ITIMER_SET

每进程一条，固定三项：REAL、VIRTUAL、PROF。每项保存：

```text
u32 kind
u32 flags
u64 interval_ns
u64 remaining_ns
```

内核 snapshot 统一使用纳秒；converter 转换到 CRIU `itimer_entry` 的 sec/usec。如果无法无损转换为微秒，返回 unsupported，不进行隐式舍入。

剩余时间使用 CRIU 语义：active timer 已过期时保存最小非零时间；真正 disarmed 的 timer 保存零。

### 3.6 POSIX_TIMER_TABLE

每进程一条，条目按 timer ID 排序。每项保存：

```text
u32 timer_id
u32 clock_id
u32 signo
u32 sigev_notify
u32 flags
u32 overrun
u32 notify_tid
u32 reserved
u64 sival_ptr
u64 interval_ns
u64 remaining_ns
```

`flags` 至少包含 `ARMED` 和 `HAS_NOTIFY_TID`。`notify_tid` 记录目标线程在当前 PID namespace 中的 TID；跨 namespace 映射留给 A7/A9 联合扩展。

对于 `SIGEV_NONE` 的 expired timer，remaining 可为零；对于需要保持 armed 状态的 timer，不能使用零值触发 CRIU 的特殊“改成 interval”路径之外的歧义。

---

## 4. 内核采集算法

### 4.1 线程集合

A6 使用 A2 freeze context 已经 pin 住的线程集合，不重新依赖一次临时线程枚举。需要为 dump 层增加只读访问器或将 freeze task set 传入 A6 collector，避免线程身份在多个 collector 之间漂移。

### 4.2 两阶段预分配

1. freeze settled 后，在 `siglock` 下统计 pending queue、POSIX timer 数量和 timer ID 集合。
2. 释放 `siglock`，以 `GFP_KERNEL` 预分配所有数组和 raw siginfo 存储。
3. 重新进入 RCU，复制 timer 指针数组。
4. 按 timer ID 排序并获取所有 `it_lock`。
5. 在持有所有 `it_lock` 后获取 `siglock`。
6. 在锁内复制 sigaction、pending、interval timer 和 POSIX timer 状态。
7. 释放 `siglock`，反向释放 `it_lock`，退出 RCU。
8. 仅在全部校验成功后执行 snapshot 文件写入。

### 4.3 锁顺序

Linux 5.10.29 的 POSIX timer callback 使用：

```text
timer->it_lock -> sighand->siglock
```

A6 必须保持相同顺序，不能在持有 `siglock` 时取得 `it_lock`。timer pointer 在 RCU 读侧保护下使用；重新取得 `siglock` 后必须检查 timer 仍属于目标 `signal_struct`。

### 4.4 一致性重试

最终复制阶段检查：

- freeze generation、task count、TID 集合不变；
- `sighand` 和 `signal` 身份不变；
- timer 数量、ID 集合和归属不变；
- queue mask、链表条目和预扫描计数一致；
- `notify_tid` 属于冻结线程集合。

失败时丢弃内存快照并有限重试；重试耗尽返回 inconsistent，不生成部分 snapshot。

### 4.5 锁内禁止事项

持有 `siglock` 或任意 `it_lock` 时禁止：

- `kmalloc(GFP_KERNEL)`；
- `kernel_write()` 或其他文件 I/O；
- 可能睡眠的路径查找、日志辅助操作或用户拷贝；
- 释放最后一个外部引用；
- 调用需要反向获取锁的 timer API。

---

## 5. Converter 两阶段模型

### 5.1 Phase 1：解析和校验

扩展 `snapshot_model` 保存 A6 record blob 和按 TID/queue scope 建立的索引。完整校验：

- A6 capability flag 与四类 record 的完整性；
- signal action 连续性和默认不可修改 signal；
- pending queue 分片、顺序、长度和 raw siginfo；
- TASK/THREAD legacy bitset 与 A6 authoritative data；
- timer ID、clock、notify、target TID、时间范围和精度；
- 线程、fd、VMA、page 等已有 A3–A5 invariants。

Phase 1 失败时不创建或修改目标 image directory。

### 5.2 Phase 2：生成 protobuf

扩展现有 `build_task_core()`、`build_thread_core()` 和 `build_timers()`：

- leader core 写真实 `sa_entry`、shared queue 和 timer table；
- 每个 thread core 写真实 private queue；
- 所有 siginfo 以 128 字节 bytes 原样写入；
- POSIX timer 按 ID 顺序生成；
- legacy snapshot 才允许使用现有默认填充逻辑。

### 5.3 事务性输出

沿用 `criu_emit_images()` 的 staging directory + fsync + atomic rename。A6 应将当前 `.a5-tmp.XXXXXX` 改成阶段无关的 `.criu-module-tmp.XXXXXX`。任意镜像失败时删除新 staging directory，不触碰已有有效输出。

---

## 6. 测试和验收

### 6.1 用户空间 ABI fixture

新增 fixture 覆盖：

- 多个自定义 handler、flags、mask、restorer；
- shared pending 和多个 private pending；
- realtime signal 顺序；
- bit-only synthetic siginfo；
- 三类 interval timer；
- POSIX periodic timer、`SIGEV_THREAD_ID`、overrun、剩余时间；
- 空 queue 和空 POSIX timer table。

负例覆盖：缺 record、duplicate record、queue 分片缺口、错误 siginfo 长度、未知 TID、重复 timer ID、非法时间和 TASK/THREAD bitset 冲突。

### 6.2 Guest fixture

新增 aarch64 静态测试程序：

- `tests/progs/sig-handlers.c`：验证 handler、mask、flags 和 restorer；
- `tests/progs/sig-pending.c`：验证 shared/private、普通/实时 signal 和投递次数；
- `tests/progs/timers.c`：验证 interval timer、POSIX timer、periodic timer、`SIGEV_THREAD_ID` 和 overrun 镜像字段。

### 6.3 Guest gate

新增或扩展：

```text
tests/a6-abi-contract.sh
tests/a6-converter-images.sh
tests/a6-unsupported.sh
tests/a6-cross-restore.sh
tests/a6-regression.sh
```

权威 gate 必须：

- 在 Lima 中构建 userspace 和 Linux 5.10.29 模块；
- 在嵌套 Linux 5.10.29/aarch64 QEMU guest 中加载模块、dump、convert；
- 使用真实 CRIU restore；
- 检查 restore 后进程仍存活并继续第二阶段行为验证；
- 检查 dmesg 无 `BUG`、`Oops`、`WARNING`、atomic sleep 或 lock inversion；
- 区分环境缺失、明确 unsupported、数据不一致和实现错误。

`timer_getoverrun()` 精确运行时数值不属于首个 A6 gate，但 overrun 必须在 snapshot 和 CRIU image 中可见且经过 fixture 校验。

---

## 7. A3/A4/A5 回归要求

A6 实现不能破坏：

- A3 minimal dump/cross-restore；
- A4 multi-thread/TLS/register restore；
- A5 regular file、pipe、UNIX stream socket；
- A5 unsupported 类型的明确拒绝和 rollback；
- legacy snapshot fixture 的 converter 兼容路径。

新的 A6 snapshot 如果缺失任一 A6 record，必须失败，不得走 A3/A5 默认状态填充。

---

## 8. 错误分类

| 条件 | 结果 |
|---|---|
| 线程/timer/queue 在采集间变化，可重试 | `-EAGAIN` / inconsistent |
| 队列或记录超过明确上限 | `-E2BIG` / unsupported |
| 不支持的 timer clock、notify 或 ABI | `-EOPNOTSUPP` |
| 内存预分配失败 | `-ENOMEM` |
| snapshot I/O 失败 | `-EIO` |
| converter 发现缺失、重复、冲突或序号缺口 | format/inconsistent |
| restore 后行为失败 | implementation failure，不能只看 CRIU 返回值 |

所有错误路径都必须清理临时 snapshot/image，不留下半套结果。
