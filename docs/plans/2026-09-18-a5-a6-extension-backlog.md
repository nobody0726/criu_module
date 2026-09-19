# A5/A6 后续扩展清单

> 本文件用于集中记录 A5、A6 首个发布范围之外的能力，防止在完成核心门禁后被遗忘。
> 这些项目不自动扩大当前阶段的完成标准。除非后续重新评审并调整计划，否则统一
> 在 A9 完成后进入扩展实施和验证。

## 1. 执行规则

当前 A 轨的发布按“核心范围 + 明确拒绝未支持状态 + 可复现验证证据”执行：

1. 首个范围之外的状态必须明确返回不支持或记录为后续任务，不能静默丢失。
2. A5/A6 的核心 gate 通过，不代表本清单中的能力已经实现。
3. A9 负责 A 轨整体集成和核心门禁；它不会自动吸收本清单的全部扩展项。
4. A9 完成后，根据依赖关系、ZDTM 失败统计和实际需求，逐项建立扩展任务、设计、
   fixture 和 Linux 5.10.29 guest 验证记录。
5. 扩展验证仍须遵守 [A3 问题与解决方法复盘](../A3-问题与解决方法复盘.md)：
   区分环境失败、数据丢失、明确不支持和实现错误；不能把 restore 返回 0 当作行为
   恢复成功。

## 2. A5 文件描述符扩展

以下项目不属于 A5 当前已确认的 pipe、常规文件和受限 UNIX socket 核心范围：

| 项目 | 当前状态 | 后续注意事项 |
|---|---|---|
| UNIX listener、pathname-bound socket、listen/backlog 语义 | 延后 | 需要验证绑定路径、监听队列和恢复顺序 |
| UNIX datagram/seqpacket | 延后 | 不能按已支持的 stream socket 路径静默处理 |
| `SCM_RIGHTS`、`SCM_CREDENTIALS` 及其他 ancillary data | 延后 | 需要完整对象图和凭据语义验证 |
| 外部 peer、跨 namespace、跨进程或不同 fd 表 | 延后 | 先解决引用闭包和参与者发现问题 |
| FIFO、文件锁和完整 socket options | 延后 | 需要单独的内核字段映射和 restore fixture |
| TCP/INET 等网络对象 | 延后 | 不纳入当前 UNIX socket 核心 gate；后续可参考 CRIU `soccr`/`TCP_REPAIR` |
| 已删除文件的 ghost-file 重建 | 延后 | 需要内容保存、路径重建和原子失败清理 |
| 同线程组独立 fd 表、外部 `CLONE_FILES` owner | 延后 | 当前只接受已确认的 fd 表闭包 |
| `F_SETOWN`/`O_ASYNC` 相关 fown 信号语义 | 转入 A6 扩展 | 需要与信号目标、线程和 pending queue 联合验证 |
| 全量 ZDTM 与 GitHub CI | 扩展验证 | 不能用定向 gate 的 PASS 代替全量结果 |

这些项目在 A5 文档中已有分散记录；本文件是统一索引，后续状态以本文件和对应的
专项设计/验证文档为准。

### `criu/soccr` 的定位

CRIU 的 `criu/soccr/` 是用户空间静态库 `libsoccr.a`，不是内核模块。它通过 Linux
TCP 的 `TCP_REPAIR` socket option 和相关 `getsockopt`/`setsockopt` 接口，在用户空间
完成 TCP 连接的暂停、状态读取和恢复，主要处理：

- TCP 状态、序号、发送/接收队列中的字节；
- MSS、window scale、timestamp 等 TCP 选项；
- repair window 和部分半关闭（FIN）状态；
- restore 时重新 bind/connect、设置队列序号并回灌队列数据。

CRIU 的 `sk-tcp.c` 负责把这些数据写入/读取 `tcp-stream.img` 等 protobuf 镜像，
`libsoccr` 只负责 TCP repair 操作和队列搬运，并不替代整个 CRIU 镜像层。

这对本项目有三点约束：

1. A5 当前不支持 TCP/INET，所以当前 A5/A6 实现不调用 `libsoccr`，也不把 TCP
   socket 当作已支持对象。
2. `libsoccr` 不能直接放进内核模块：它依赖 libc、用户空间 socket API 和
   `TCP_REPAIR`，与本项目“内核采集、userspace converter 生成 CRIU 镜像”的边界不同。
3. 将来若实施 TCP 扩展，`soccr` 可以作为 CRIU 语义和镜像字段的参考，restore 侧
   也可能复用 CRIU 已有的 TCP repair 路径；但 dump 侧仍需单独决定是增加内核 TCP
   状态采集记录，还是设计受控的用户空间协作采集。该决定尚未做出。

因此，当前计划状态是“TCP/INET 能力已列入 A5 后续扩展 backlog，`soccr` 作为参考
实现已记录，但尚未承诺集成或实现”。在 A9 后启动该扩展前，必须先补充 TCP 状态
ABI、网络冻结/隔离策略、队列一致性、镜像映射和 Linux 5.10.29 guest cross-restore
fixture。

## 3. A6 信号与定时器扩展

### 3.1 A6 首个范围（用于边界对照）

A6 首个范围是已确认的核心信号和定时器集合：

- 进程共享的 `sigaction` 表；
- 每线程 blocked mask；
- 每线程 private pending queue；
- 进程 shared pending queue，保留原始 `siginfo_t`；
- `ITIMER_REAL`、`ITIMER_VIRTUAL`、`ITIMER_PROF`；
- POSIX timers 的周期、剩余时间、overrun 和 `SIGEV_THREAD_ID` 目标；
- userspace converter 生成 CRIU protobuf/image，内核只采集结构化数据；
- 在 Linux 5.10.29/aarch64 nested QEMU guest 中执行真实 CRIU restore 验证。

### 3.2 明确延后的 A6 项目

下列能力不计入 A6 首个 gate。若它们在实现过程中被目标进程触发，必须明确拒绝或
记录为后续任务，不能降级成默认值：

| 项目 | 延后原因/边界 | 计划归属 |
|---|---|---|
| A5 转入的 `F_SETOWN`/`O_ASYNC` fown 信号完整恢复 | 涉及 fd owner、信号目标和异步投递的跨 A5/A6 语义 | A6 extension（A9 后） |
| `signalfd`、`timerfd` 等 fd 绑定的信号/定时器状态 | 同时依赖 fd 对象图和信号/定时器状态，超出 A6 核心镜像集合 | A5/A6 联合扩展（A9 后） |
| 跨进程、跨 namespace 的信号目标解析与恢复 | 依赖 A7 进程树、PID/session/namespace 上下文 | A7/A9 后的联合扩展 |
| 尚未在 A6 ABI 中定义的特殊 timer/clock 语义 | 缺少稳定的 CRIU 字段映射和冻结期间语义契约 | A6 extension（重新设计后） |
| 与外部任务或未纳入 dump 闭包对象交互的 pending/timer 状态 | 无法证明引用闭包和恢复顺序 | A7/A8/A9 后按用例处理 |
| 信号/定时器相关的完整 ZDTM、组合场景和 GitHub CI | 属于广泛回归验证，不是首个能力 gate | A9 后扩展验证 |

这里的“延后”不是永久不支持：在 A9 后必须先为每个项目补充范围、ABI 映射、错误
分类、测试 fixture 和 guest 实测命令，再决定实现顺序。

## 4. 进入扩展实施的条件

某个条目只有同时满足以下条件，才可从 backlog 转入实现：

- 已确认它不破坏 A3/A4/A5/A6 核心行为和已有 ABI 兼容性；
- 已明确内核采集、userspace converter、CRIU image 字段和 restore 语义的边界；
- 已准备好至少一个可重复的 Linux 5.10.29/aarch64 guest fixture；
- 已定义成功、明确不支持、数据丢失、环境失败和实现错误的判定；
- 已有独立设计/实施计划，并在完成后补充真实 restore 证据。

## 5. 当前状态

- A5 核心范围：已完成并发布；本文件中的 A5 条目仍是后续扩展。
- A6 核心范围：已完成实现，并通过 Lima host-side 回归与 Linux 5.10.29/aarch64
  nested QEMU guest 的定向 cross-restore 门禁；本文件中的 A6 条目仍属于后续扩展，
  不因核心 gate 通过而自动进入当前发布范围。
- A9：尚未详细设计；在 A9 完成前不以本清单中的条目为完成阻塞，除非新的核心
  集成证据表明某项是 A 轨门禁的必要依赖。
