# 原理 02 —— 怎么让一个进程停下来

> 被引用于:[A2](../steps/A2-freeze.md)

---

## 1. 为什么必须先停

dump 要做的事是「给进程的状态拍一张照」。如果进程还在跑,拍出来的东西会是这样:

```
时刻 t0:  读到 fd 表 —— 此时有 fd 3, 4, 5
时刻 t1:  进程 close(4), open() 得到 fd 4 指向别的文件
时刻 t2:  读到内存 —— 此时内存里的数据结构已经指向新的 fd 4 的语义
```

拍出来的镜像里,fd 表是 t0 的,内存是 t2 的。**这两份状态从未同时存在过。**

恢复出来的进程会处于一个逻辑上不可能的状态。这不是「有点误差」,是**根本性的
不一致**:比如内存里的一个数据结构说「我的日志文件在 fd 4」,而 fd 表里的 4 现在
是个 socket。

**所以「冻结先行」是 dump 侧唯一一条真正的强顺序依赖。** 其他所有采集步骤
(内存、fd、信号、定时器)之间都没有顺序要求,可以任意排列甚至并行。

---

## 2. 冻结的三个要求

一个合格的冻结机制必须满足:

| 要求 | 为什么 |
|---|---|
| **不可被目标观测到** | 否则目标可能改变行为,dump 到的状态就不是「正常运行时的状态」 |
| **不改变任何要 dump 的状态** | 冻结本身不能成为状态的一部分 |
| **完全可逆** | dump 失败时必须能把进程还原成什么都没发生过 |

第一条最容易被忽视,也是最常见的错误来源。

---

## 3. 为什么不能用 `SIGSTOP`

最直觉的做法是 `kill(pid, SIGSTOP)`。它有**三个**互相独立的致命问题。

### 3.1 它是可观测的

`SIGSTOP` 会改变进程的状态为 `TASK_STOPPED`。这个状态:

- 父进程的 `waitpid(WUNTRACED)` 会返回,`WIFSTOPPED(status)` 为真
- `/proc/PID/stat` 的状态字段变成 `T`
- 进程被 `SIGCONT` 唤醒后,如果它安装了 `SIGCONT` handler,handler 会被调用

**父进程可能会因此改变行为。** 一个用 `waitpid` 监控子进程的父进程,会认为子进程
被停了,可能打日志、可能重启它、可能自己也退出。

### 3.2 `SIGCONT` 会吃掉真正待处理的 `SIGSTOP`

这条最阴险。Linux 的语义是:**`SIGCONT` 会清除所有待处理的 stop 类信号**
(`SIGSTOP`/`SIGTSTP`/`SIGTTIN`/`SIGTTOU`),反之 `SIGSTOP` 也会清除待处理的
`SIGCONT`。

所以如果目标进程在你 dump 之前**本来就有一个待处理的 `SIGSTOP`**(比如用户刚按了
Ctrl-Z,信号还没被处理),那么:

```
1. 你发 SIGSTOP  → 进程停下
2. 你 dump
3. 你发 SIGCONT  → 进程恢复运行,但那个「本来的 SIGSTOP」被一起清除了
```

**你销毁了一个待处理信号。** 进程本该停下的,现在继续跑了。这违反了「完全可逆」。

而且这个 bug 几乎不可能在测试里偶然发现 —— 它需要恰好在 dump 时有一个待处理的
stop 信号。

### 3.3 无法区分「本来就停着」和「被我停的」

如果目标进程在你动手之前**已经**是 `TASK_STOPPED`(用户按过 Ctrl-Z),那么:

- 你发 `SIGSTOP`:没有可观测变化
- 你 dump
- 你发 `SIGCONT`:**进程开始运行了**,而它本来是停着的

你把一个停着的进程变成了运行的进程。同样违反「完全可逆」。

要正确处理,你必须先判断它是否已经停着 —— 而这个判断本身有竞态(你读
`/proc/PID/stat` 的瞬间和你发信号的瞬间之间,状态可能变)。

**三个问题叠加,`SIGSTOP` 方案不可修补。** 这不是实现质量问题,是机制选择错误。

---

## 4. `PTRACE_SEIZE`:CRIU 的选择

CRIU 用的是 `PTRACE_SEIZE`(`criu/criu/seize.c`)。

### `PTRACE_SEIZE` vs `PTRACE_ATTACH`

| | `PTRACE_ATTACH`(老) | `PTRACE_SEIZE`(新) |
|---|---|---|
| 附加时是否停止目标 | **是**,发一个 `SIGSTOP` | **否**,目标继续运行 |
| 是否产生 group-stop 可观测状态 | 是 | 否 |
| 是否干扰待处理信号 | 是 | 否 |
| 停止目标的方式 | 靠 `SIGSTOP` | `PTRACE_INTERRUPT`,不用信号 |
| detach 时能否保持停止 | 困难 | 可以(`PTRACE_LISTEN`) |

`PTRACE_SEIZE` 是 Linux 3.4 加的,**它加进内核的动机之一就是让 C/R 成为可能。**
它解决了 `PTRACE_ATTACH` 的全部三个问题:附加时不停止、停止时不用信号、
能区分和保持原有的停止状态。

CRIU 之所以必须用 ptrace(而不只是「停住」),是因为它接下来还要**注入 parasite
代码**——它需要在目标进程里执行代码来读那些用户态没有只读接口的状态
(信号处理表、定时器)。ptrace 提供了写目标内存和改目标寄存器的能力。

**内核模块不需要这一步。** 那些状态在内核里直接可读,这是内核模块最大的一个
优势(详见 [A6](../steps/A6-signals-timers.md))。

### CRIU 的竞态问题

`criu/criu/seize.c:973` 附近有一段处理这个情况:CRIU 逐个 `PTRACE_SEIZE` 进程树里
的任务,但在它 seize 前几个的同时,**已经被 seize 的任务可能 fork 出新的子进程**。

于是 CRIU 必须反复扫描进程树,直到某一轮没有发现新任务。这是一个收敛循环,
而不是一个确定性的操作。

---

## 5. cgroup freezer:更适合内核模块的方案

内核模块有第三个选择:cgroup freezer。

### 原理

把目标进程(整个 thread group)放进一个 freezer cgroup,写 `FROZEN`。内核会在
每个任务下一次进入内核态时把它挂起在 `__refrigerator()` 里。

```
/sys/fs/cgroup/freezer/<name>/freezer.state  ←  写 "FROZEN" / "THAWED"
```

(cgroup v2 用 `cgroup.freeze`,写 `1` / `0`)

### 为什么它更好

| 性质 | 说明 |
|---|---|
| **原子地覆盖一组任务** | 整个 cgroup 一起冻。**新 fork 出来的子进程自动进入冻结状态** |
| 不使用信号 | 完全绕开了 `SIGSTOP` 的三个问题 |
| 不使用 ptrace | 不改变 tracer 关系,不干扰真正在 debug 这个进程的 gdb |
| 状态不可被目标观测 | 目标只是「没有被调度」,它自己无法察觉 |
| 天然覆盖所有线程 | cgroup 成员是 task,一个 thread group 全部在里面 |

**「新 fork 的子进程自动被冻结」这一条,从根上消灭了 CRIU 那个反复扫描的竞态。**
CRIU 也支持这个模式(`--freeze-cgroup`),而且这是它推荐用于容器场景的方式。

### 代价

必须记录并恢复目标原来所属的 cgroup。把一个进程移进你新建的 freezer cgroup,
就把它从原来的 cgroup 里移出来了 —— 那可能是一个有资源限制的 cgroup,移出去
意味着限制失效。

**这是一个「冻结本身改变了要 dump 的状态」的例子**,违反了第 2 节的第二条要求。
所以:

```
1. 记录目标当前所属的每个 cgroup 层级的路径
2. 移入 freezer cgroup,冻结
3. dump
4. 解冻,移回原来的 cgroup
```

第 4 步必须在所有错误路径上都执行。

---

## 6. 「冻结完成」不等于「可以读寄存器了」

这是一个容易漏掉的细节,而且它的症状很难诊断。

写 `FROZEN` 之后,函数**立即返回**。但此时任务可能:

- 还在用户态执行指令(它要等到下一次进入内核态才会被冻住)
- 正在某个系统调用中途

**这两种情况下 `task_pt_regs(task)` 里的值都是不可靠的。** 只有当任务真正被
调度出去、上下文被完整保存到内核栈上时,`task_pt_regs()` 才反映它的真实寄存器。

所以必须轮询等待:

```c
	/* FROZEN is set asynchronously: a task still running in userspace only
	 * freezes at its next kernel entry. Registers are not valid until every
	 * task has actually been scheduled out.
	 */
	while (task_is_running(t)) {
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		schedule_timeout_interruptible(1);
	}
```

**必须有超时。** 一个纯用户态死循环的任务(不做任何系统调用)理论上要等到时钟
中断才会进内核 —— 那很快,但如果内核配了 `CONFIG_NO_HZ_FULL` 且该 CPU 是
nohz_full CPU,时钟中断可能被关掉,等待时间会显著变长。

而且这个等待必须覆盖**每一个线程**,不只是 group leader。

### `task_pt_regs` 是个宏

一个实用的小知识:`task_pt_regs()` 在 x86 上定义于
`arch/x86/include/asm/processor.h:763` 附近,**它是一个宏**:

```c
#define task_pt_regs(task) \
({									\
	unsigned long __ptr = (unsigned long)task_stack_page(task);	\
	__ptr += THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;		\
	((struct pt_regs *)__ptr) - 1;					\
})
```

它就是「内核栈顶往下一个 `pt_regs` 的大小」。**因为是宏,不需要任何导出符号** —— 
这对内核模块很重要,`EXPORT_SYMBOL` 的限制在这里不适用。

---

## 7. 三种方案对比

| | `SIGSTOP` | `PTRACE_SEIZE` | cgroup freezer |
|---|---|---|---|
| 可观测性 | 高(`WIFSTOPPED`) | 低 | **最低** |
| 干扰待处理信号 | **是** | 否 | 否 |
| 能否区分已停止的任务 | **否** | 是 | 是(不关心) |
| 覆盖新 fork 的子进程 | 否 | 否(需重扫) | **是** |
| 覆盖所有线程 | 需逐个 | 需逐个 | **自动** |
| 干扰已有的 debugger | 否 | **是** | 否 |
| 需要额外恢复什么 | —— | —— | 原 cgroup 归属 |
| 内核模块可用 | 是 | 困难 | **是** |

**本项目选 cgroup freezer。**

---

## 8. 延伸阅读

- `criu/criu/seize.c` —— CRIU 的冻结实现,两条路径(ptrace / freezer)都在里面
- `kernel/freezer.c` —— `__refrigerator()`,任务实际被挂起的地方
- `kernel/cgroup/legacy_freezer.c`(v1)/ `kernel/cgroup/freezer.c`(v2)
- `man 2 ptrace` 的 `PTRACE_SEIZE` / `PTRACE_INTERRUPT` / `PTRACE_LISTEN` 段落 —— 
  写得意外地清楚,值得完整读一遍
- [01-process-anatomy](01-process-anatomy.md) —— 为什么状态一致性这么重要
