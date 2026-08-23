# A4 —— 多线程

**工期:** 1-2 周 · **前置:** A3 · **产出:** 每线程 `core-$tid.img`,多线程进程可交叉恢复

> 相关原理:[05-registers-and-sigframe](../principles/05-registers-and-sigframe.md)、
> [06-pid-and-session](../principles/06-pid-and-session.md)

---

## 1. 设计思路

### 线程和进程在 dump 侧的差别比想象中小

Linux 里线程就是共享了若干资源的 task。共享什么由 `clone()` 的 flag 决定:
`CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FILES`。

所以 dump 侧的工作量增量其实很小:

| 资源 | 线程间是否共享 | dump 一次还是每线程一次 |
|---|---|---|
| 地址空间(`mm`) | 共享 | **一次**(per thread group) |
| fd 表(`files`) | 共享 | **一次** |
| 信号处理表(`sighand`) | 共享 | **一次** |
| 寄存器 | **不共享** | 每线程 |
| 信号掩码(`blocked`) | **不共享** | 每线程 |
| pending 信号 | **各自一份 + 共享一份** | 见 4.2 |
| TLS(`fs_base`) | **不共享** | 每线程 |
| 栈 | 物理上在同一个 mm 里,但每线程一段 | 随 mm 一起 |
| `set_child_tid` / `clear_child_tid` | **不共享** | 每线程 |

**A4 的核心工作就是把「每线程一次」的那几行拆出来。** A3 已经写好的 mm/files
dump 代码一行都不用改 —— 这正是 A3 把接口设计成 per-task-group 的回报。

### 遍历线程

```c
	/* for_each_thread walks the thread group. Must hold RCU or the
	 * tasklist lock; RCU is enough for a read-only walk.
	 */
	rcu_read_lock();
	for_each_thread(group_leader, t) {
		/* t->pid is the TID, t->tgid is the process id */
		...
	}
	rcu_read_unlock();
```

注意内核里的命名与用户空间**相反**,这是一个经典混淆源:

| 内核字段 | 用户空间的 `getpid()`/`gettid()` |
|---|---|
| `task->pid` | `gettid()` —— 线程 id |
| `task->tgid` | `getpid()` —— 进程 id |

写代码时把这两个搞混,症状是「单线程时一切正常,多线程时线程 id 全是主线程的」。

### 冻结必须覆盖所有线程

A2 用 cgroup freezer,天然按进程冻(整个 thread group 一起进 cgroup),所以这条
自动满足。但 A2 的 `criu_freeze_settled()` 要检查**每个**线程都不在跑 —— 回头
确认 A2 的实现是遍历了 `for_each_thread` 而不是只看 group leader。

**这是 A4 第一天该做的事:回去验证 A2 的 settled 判定覆盖了所有线程。** A2 的
测试用例 2(8 线程冻结)应该已经覆盖了,如果当时是只看 leader 而测试碰巧过了,
这里就会暴露。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 镜像布局

- `pstree.img` 的 `pstree_entry.threads` 是一个**重复字段**,列出所有 TID
  (`criu/images/pstree.proto`)
- 每个线程一个 `core-$tid.img`。**主线程的 `core-$pid.img` 和线程的格式完全相同**
- `core.proto` 里的 `thread_core_entry` 装每线程独有的东西;
  `task_core_entry` 装 per-process 的东西(**只有 group leader 的 core 里有**)

这个「同一个 message 里一部分字段只对 leader 有效」的设计有点别扭,但必须照抄。

### 2.2 CRIU 怎么读线程寄存器

CRIU 对每个线程单独 `PTRACE_GETREGSET`。它必须逐个 attach,这是它的一大开销来源。

**内核模块这里省事得多:`task_pt_regs(t)` 对每个线程直接可用**,不需要任何 attach。
这是继「不需要 parasite」之后,内核模块的第二个真实优势。

### 2.3 restore 侧线程是怎么建的(理解用,A4 不实现)

`criu/criu/pie/restorer.c:2655` 附近,线程创建也用 `clone3` + `set_tid`:

```c
				c_args.set_tid_size = 1;
```

并在 2687 附近有和进程一样的 TID 断言。所以**线程 TID 也是精确恢复的**,不是
best-effort。这解释了为什么 A4 必须把 TID 准确写进镜像 —— 写错了 restore 会
直接断言失败,不会将错就错。

**要抄的:** `threads` 字段、per-thread core 文件、`thread_core_entry` 与
`task_core_entry` 的划分。
**不抄的:** 逐线程 ptrace attach。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/dump_threads.c`
- `tests/progs/threads.c`

**Modify:**
- `kernel_module/checkpoint/dump_core.c` —— 拆成 leader 部分和 per-thread 部分
- `kernel_module/checkpoint/dump_misc.c` —— `pstree_entry.threads` 填充
- `kernel_module/checkpoint/dump.c` —— 编排里加线程循环

**Interfaces produced:**

```c
/* Iterate every task in vpid's thread group. fn is called with RCU held, so
 * it must not sleep. Copy what you need and do the I/O outside. */
typedef int (*criu_thread_fn)(struct task_struct *t, void *arg);
int criu_walk_threads(struct task_struct *leader, criu_thread_fn fn, void *arg);

/* Write core-$tid.img for one thread. is_leader selects whether the
 * task_core_entry (process-wide) part is emitted. */
int criu_dump_thread_core(struct criu_dump_ctx *ctx, struct task_struct *t,
			  bool is_leader);
```

**`fn` 在 RCU 下被调用所以不能睡眠** —— 但写文件是要睡眠的。所以实现必须是
两阶段:RCU 下把线程指针数组抓出来(`get_task_struct` 各加一个引用),放锁,
然后逐个写文件。这个约束要写在头文件的注释里,否则后来的人会在回调里直接
`kernel_write` 然后被 `DEBUG_ATOMIC_SLEEP` 打脸。

---

## 4. 关键实现要点

### 4.1 TLS:`fs_base` 与 `gs_base`

x86_64 上 TLS 通过 `%fs` 段寄存器实现,基址在 `task->thread.fsbase`。
`core-x86.proto` 里有对应字段。

陷阱:5.10 上读 `fsbase` 要用 `x86_fsbase_read_task(t)` 而不是直接读
`t->thread.fsbase` —— 因为如果线程正在 CPU 上跑,真值在 MSR 里。A2 保证了任务
已停止,所以直接读字段**通常**对,但用 helper 更安全。**S0 要顺手验证
`x86_fsbase_read_task` 是否导出;若否,在任务已停止的前提下直接读字段并在注释里
记录这个假设。**

### 4.2 pending 信号:两个队列

```c
	/* Per-thread queue: signals sent with tgkill() to this specific tid. */
	&t->pending
	/* Shared queue: signals sent to the process as a whole with kill(). */
	&t->signal->shared_pending
```

`shared_pending` 只 dump 一次(在 leader 的 core 里),`pending` 每线程各一份。
搞混的症状:恢复后信号被投递多次(每线程一次),或者丢失。

CRIU 的对应镜像是 `signal-$pid.img`(共享队列)和 `psigfd-$tid.img` 之类。
准确的文件名以 `crit x /tmp/ref-imgs/ ps` 的实际输出为准 —— **不要凭记忆写,
跑一遍真 criu 看它产出什么。**

### 4.3 `clear_child_tid`

`task->clear_child_tid` 是 `pthread_join` 依赖的机制:线程退出时内核往这个地址
写 0 并 futex-wake。它必须 dump,否则恢复后 `pthread_join` 永久阻塞。

`core.proto` 的 `thread_core_entry` 里有对应字段。**这是一个「不 dump 也能过大多数
测试,但会让 `pthread_join` 静默挂死」的字段**,属于必须主动想到的类别。

---

## 5. 如何测试

### 5.1 目标程序

```c
/* threads.c: N threads, each with its own TLS value, its own stack pattern,
 * and its own counter. Every thread self-verifies, so a restore that mixes up
 * per-thread state is reported by the affected thread itself.
 *
 *   gcc -static -O0 -pthread -o threads threads.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define NTHREADS 8

/* __thread makes this per-thread storage: the whole point of the test. */
static __thread unsigned long tls_magic;
static __thread char tls_pad[512];

static volatile int corrupt;

static void *worker(void *arg)
{
	unsigned long idx = (unsigned long)arg;
	unsigned long counter = 0;	/* lives on this thread's stack */
	char stack_pattern[1024];

	tls_magic = 0x71500000UL + idx;
	memset(tls_pad, (int)idx, sizeof(tls_pad));
	memset(stack_pattern, (int)(0x40 + idx), sizeof(stack_pattern));

	for (;;) {
		/* Each thread proves its own TLS and stack survived. */
		if (tls_magic != 0x71500000UL + idx) {
			printf("THREAD %lu TLS CORRUPT: %lx\n", idx, tls_magic);
			corrupt = 1;
		}
		if ((unsigned char)tls_pad[0] != (unsigned char)idx) {
			printf("THREAD %lu TLS_PAD CORRUPT\n", idx);
			corrupt = 1;
		}
		if ((unsigned char)stack_pattern[0] !=
		    (unsigned char)(0x40 + idx)) {
			printf("THREAD %lu STACK CORRUPT\n", idx);
			corrupt = 1;
		}
		printf("thread=%lu tid=%d counter=%lu\n",
		       idx, (int)gettid(), counter++);
		fflush(stdout);
		sleep(1);
	}
	return NULL;
}

int main(void)
{
	pthread_t th[NTHREADS];
	unsigned long i;

	printf("pid=%d nthreads=%d\n", getpid(), NTHREADS);
	fflush(stdout);

	for (i = 0; i < NTHREADS; i++)
		if (pthread_create(&th[i], NULL, worker, (void *)i))
			return 1;

	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	return corrupt ? 2 : 0;
}
```

**每线程自检 TLS + 自检栈 + 自己的 counter**,三样加起来能抓住:TLS 恢复到错误
线程、栈内容错乱、寄存器串线(counter 在寄存器或栈上)。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 8 线程进程能被 `criu restore` 恢复 | 退出码 0 |
| 2 | 恢复后所有 8 个 TID **与原来相同** | `ls /proc/$PID/task/` 集合相等 |
| 3 | 每线程 TLS 正确 | 无 `TLS CORRUPT` |
| 4 | 每线程栈正确 | 无 `STACK CORRUPT` |
| 5 | 每线程 counter 从各自的值继续 | 8 个 counter 都递增且不归零 |
| 6 | 线程数 = 1 时行为不变 | A3 的测试仍然通过(回归) |
| 7 | 128 线程 | 能完成,`pages` 文件里栈区域不重复 |
| 8 | dump 期间某线程退出 | 不 oops;报错或正确处理 |
| 9 | 每线程独有的信号掩码 | 恢复后 `/proc/$PID/task/$TID/status` 的 `SigBlk` 一致 |
| 10 | `pthread_join` 在恢复后仍能工作 | 见下 |
| 11 | 持有 `pthread_mutex` 时被 dump | 恢复后能正常 unlock |

用例 2 是关键。TID 集合相等,而不只是数量相等 —— 数量对但 TID 错的实现能通过
一个粗糙的测试,而实际上会让所有存了 TID 的用户态结构(robust futex、
`pthread_mutex` 的 owner)全部失效。

用例 10 的构造:主线程 `pthread_join` 一个会在 30 秒后退出的线程,checkpoint
在等待期间进行。恢复后那个线程正常退出,主线程的 `pthread_join` 必须返回。
**这个用例专门验证 4.3 的 `clear_child_tid`。**

用例 11 的构造:一个线程 lock 住 mutex 并 sleep,checkpoint,restore,然后
unlock。如果 mutex 里存的 owner TID 没恢复对,`pthread_mutex_unlock` 会返回
`EPERM`。**这是 TID 必须精确恢复的最直接证据。**

### 5.3 ZDTM 增量

A4 完成后应该能解锁的 zdtm 测试(加入 allowlist 前逐个验证):

```
zdtm/static/pthread00
zdtm/static/pthread01
zdtm/static/pthread02
zdtm/static/tls00
zdtm/static/tls01
```

---

## 6. 完成标准

- [ ] 11 个用例通过,含 10、11 两个难点
- [ ] A3 的所有测试仍然通过(单线程回归)
- [ ] 回去确认 A2 的 `criu_freeze_settled` 覆盖所有线程,而非只看 leader
- [ ] `criu_walk_threads` 的「回调在 RCU 下、不可睡眠」约束写进头文件注释
- [ ] allowlist 增加至少 3 个 pthread/tls 测试
