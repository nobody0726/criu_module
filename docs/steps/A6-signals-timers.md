# A6 —— 信号与定时器

**工期:** 1-2 周 · **前置:** A3 · **产出:** `sigacts-*.img` / `timer*.img`

> 相关原理:[05-registers-and-sigframe](../principles/05-registers-and-sigframe.md)

---

## 1. 设计思路

### 这一步是内核模块优势最大的一步

CRIU 拿信号处理表必须靠 parasite:`sigaction` 表在 `task->sighand->action[]` 里,
用户空间**没有任何只读接口**能拿到它。`/proc/PID/status` 只给你三个位掩码
(`SigBlk`/`SigIgn`/`SigCgt`),完全不含 handler 地址和 `sa_flags`。

所以 CRIU 必须:注入 parasite → 让目标进程自己对 64 个信号各调一次
`sigaction(sig, NULL, &old)` → 把结果传回来 → 拔出 parasite。

**内核模块一行就拿到:**

```c
	spin_lock_irq(&task->sighand->siglock);
	for (i = 0; i < _NSIG; i++)
		snapshot[i] = task->sighand->action[i];
	spin_unlock_irq(&task->sighand->siglock);
```

这是「消除 parasite」这个论点最干净的证据。如果整个项目只做一件事来证明内核模块
有价值,做这一步。

### 三类东西,分开处理

| | 存在哪 | 共享性 | 镜像 |
|---|---|---|---|
| **sigaction 表** | `task->sighand->action[]` | 线程间**共享** | `sigacts-$pid.img`,每进程一份 |
| **信号掩码** | `task->blocked` | 每线程独有 | 在 `core-$tid.img` 里 |
| **pending 信号** | `task->pending`(私有)+ `task->signal->shared_pending`(共享) | 混合 | `signal-$pid.img` / `psigfd` |
| **POSIX 定时器** | `task->signal->posix_timers` | 进程级 | `timer-$pid.img` |
| **interval 定时器** | `task->signal->it[]` | 进程级 | 在 `core-$pid.img` 的 `itimer` 字段 |

`sighand` 共享而 `blocked` 不共享,这个不对称是 A6 最容易搞混的地方,而且症状很
隐蔽:恢复后信号被投递到错误的线程,或者某个线程该屏蔽的信号没屏蔽。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 镜像格式

- `criu/images/siginfo.proto` —— pending 信号的 `siginfo_t` 裸字节
- `criu/images/timer.proto` —— POSIX 定时器
- `criu/images/core.proto` 的 `task_timers_entry` —— itimer(`ITIMER_REAL` /
  `ITIMER_VIRTUAL` / `ITIMER_PROF`)
- sigaction 表在 `sigacts-$pid.img`,格式是 64 个 `sa_entry` 顺序排列

参照 `criu/criu/sigframe.c` 和 `criu/criu/parasite-syscall.c` 里
`parasite_dump_sigacts_seized()`。

### 2.2 pending 信号的 siginfo 是裸字节

`siginfo_t` 是个 128 字节的 union,内容取决于 `si_code`。CRIU **不解析它**,
原样存 128 字节。这是对的做法 —— 解析它意味着要覆盖所有 `si_code` 组合,而任何
一个漏掉都会静默丢信息。

**A6 照抄:`memcpy` 128 字节,不解析。**

内核里 pending 信号在链表上:

```c
	/* Both queues must be walked: kill() lands in shared_pending,
	 * tgkill() lands in the per-thread pending queue.
	 */
	list_for_each_entry(q, &task->pending.list, list) {
		/* q->info is the siginfo_t */
	}
```

### 2.3 定时器:剩余时间还是绝对时间

这是 A6 唯一一个真正的语义难题。

一个 `timer_settime` 设的定时器,内核里存的是**绝对到期时刻**(基于
`CLOCK_MONOTONIC` 等)。但 restore 可能发生在几小时后,甚至另一台机器上。所以:

- 存绝对时刻 → restore 后立即到期(时间已经过了)
- 存剩余时间 → 语义正确,但 dump 到 restore 之间的时间被「冻结」了

**CRIU 选择存剩余时间。** 参照 `criu/images/timer.proto` 的字段名就能看出来。
这是一个刻意的语义决定:checkpoint/restore 期间的时间对进程而言不存在。

A6 照抄这个决定。计算方法:

```c
	/* Store time REMAINING, not the absolute expiry: the process may be
	 * restored hours later, and an absolute deadline would fire instantly.
	 * CRIU makes the same choice (see images/timer.proto).
	 */
	remaining = ktime_sub(hrtimer_get_expires(&timr->it.real.timer),
			      hrtimer_cb_get_time(&timr->it.real.timer));
	if (ktime_to_ns(remaining) < 0)
		remaining = 0;	/* already expired, will fire on restore */
```

`remaining < 0` 的情况是真实的:定时器已经到期但信号还没投递(比如任务被冻结了)。
归零而不是留负数,让它 restore 后立刻触发 —— 这与不冻结时的行为最接近。

### 2.4 `sa_restorer` 是个陷阱

x86_64 上 `struct sigaction` 有个 `sa_restorer` 字段,指向 libc 里的一小段代码
(它调 `rt_sigreturn`)。它必须被 dump 和恢复,否则 handler 返回时会跳到野地址。

但它是**libc 内部实现细节**,地址在 libc 的代码段里。因为 A3 的目标是静态链接,
而且 restore 时整个地址空间是按原样恢复的,所以这个地址仍然有效 —— **只要原样
存原样恢复,不要试图重新计算它。**

内核里它在 `task->sighand->action[i].sa.sa_restorer`。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/dump_signals.c`
- `kernel_module/checkpoint/dump_timers.c`
- `tests/progs/sig-handlers.c` / `sig-pending.c` / `timers.c`

**Modify:**
- `kernel_module/checkpoint/dump_core.c` —— 加 `blocked` 和 itimer 字段
- `kernel_module/checkpoint/dump_threads.c` —— 每线程 pending 队列

**Interfaces produced:**

```c
/* Snapshot the shared sigaction table. Copies under siglock into caller
 * memory, because the caller will do image I/O and must not hold a spinlock
 * while sleeping. */
int criu_snapshot_sigacts(struct task_struct *task,
			  struct k_sigaction *out /* [_NSIG] */);

/* Walk one pending queue. shared selects signal->shared_pending vs
 * task->pending. Callback runs under siglock: copy, do not sleep. */
typedef int (*criu_siginfo_fn)(const kernel_siginfo_t *info, void *arg);
int criu_walk_pending(struct task_struct *task, bool shared,
		      criu_siginfo_fn fn, void *arg);

/* POSIX timers, converted to remaining-time form. */
int criu_dump_posix_timers(struct criu_dump_ctx *ctx,
			   struct task_struct *task);
```

两个接口的注释都强调「持 spinlock,不能睡眠」。这是 A6 的主要技术风险:
`siglock` 是 spinlock,而写文件会睡眠。**必须先快照到内存,放锁,再写文件。**
`CONFIG_DEBUG_ATOMIC_SLEEP` 会抓住违反,这就是为什么 `build-kernel.sh` 开了它。

---

## 4. 关键实现要点

### 4.1 `_NSIG` 是 64,但数组下标从 0 开始

信号编号从 1 开始(`SIGHUP` = 1),`action[]` 数组下标是 `sig - 1`。
`action[0]` 对应 `SIGHUP`。差一错误在这里代价很高:所有 handler 整体偏移一位,
症状是「`SIGINT` 的 handler 变成了 `SIGQUIT` 的」。

### 4.2 `SIGKILL` / `SIGSTOP` 不能有 handler

它们的 `action[]` 项永远是默认值。dump 时照原样存,restore 时 CRIU 会跳过它们
(`sigaction` 对它们会返回 `EINVAL`)。**不要试图跳过它们不存** —— 格式要求
64 项都在。

### 4.3 定时器的 signal 目标

POSIX 定时器可以设成「信号发给某个特定线程」(`SIGEV_THREAD_ID`)。
`timr->it_pid` 记录目标。这个必须 dump,否则恢复后信号发给了错误的线程。

```c
	/* SIGEV_THREAD_ID timers target one specific thread; without this the
	 * signal goes to the process and any thread may take it.
	 */
	if (timr->it_sigev_notify & SIGEV_THREAD_ID)
		entry.notify_thread_id = pid_vnr(timr->it_pid);
```

### 4.4 遍历 posix_timers

```c
	spin_lock_irq(&task->sighand->siglock);
	list_for_each_entry(timr, &task->signal->posix_timers, list) {
		/* snapshot into a preallocated array */
	}
	spin_unlock_irq(&task->sighand->siglock);
```

数组要**预分配**,因为持锁时不能 `kmalloc(GFP_KERNEL)`。做法:先数一遍数量
(持锁),放锁,分配,再持锁填充,并检查数量没变。任务已冻结所以数量不会变,
但断言要写上。

---

## 5. 如何测试

### 5.1 信号测试程序

```c
/* sig-handlers.c: install a distinguishable handler for several signals, set
 * a non-trivial blocked mask, and let the process verify its own signal state
 * after restore.
 *
 *   gcc -static -O0 -o sig-handlers sig-handlers.c
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t got[NSIG];

static void handler(int sig, siginfo_t *si, void *uc)
{
	got[sig]++;
}

int main(void)
{
	struct sigaction sa, back;
	sigset_t mask, oldmask;
	int sigs[] = { SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN + 1 };
	unsigned i, tick = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGTERM);	/* distinctive sa_mask */

	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		if (sigaction(sigs[i], &sa, NULL))
			return 1;

	/* SIGPIPE ignored, SIGCHLD default: three different dispositions in
	 * one process, so a table that gets shifted or truncated shows up. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);

	sigemptyset(&mask);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGALRM);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);

	printf("pid=%d ready\n", getpid());
	fflush(stdout);

	for (;;) {
		/* Verify our own signal dispositions survived. */
		if (sigaction(SIGUSR1, NULL, &back))
			return 1;
		if (back.sa_sigaction != handler) {
			printf("HANDLER LOST for SIGUSR1\n");
			fflush(stdout);
			return 2;
		}
		if (!(back.sa_flags & SA_SIGINFO) ||
		    !sigismember(&back.sa_mask, SIGTERM)) {
			printf("SA_FLAGS/SA_MASK LOST\n");
			fflush(stdout);
			return 3;
		}
		if (sigaction(SIGPIPE, NULL, &back))
			return 1;
		if (back.sa_handler != SIG_IGN) {
			printf("SIG_IGN LOST for SIGPIPE\n");
			fflush(stdout);
			return 4;
		}
		sigprocmask(SIG_BLOCK, NULL, &oldmask);
		if (!sigismember(&oldmask, SIGHUP) ||
		    !sigismember(&oldmask, SIGALRM)) {
			printf("BLOCKED MASK LOST\n");
			fflush(stdout);
			return 5;
		}
		printf("tick=%u usr1=%d rt=%d\n", tick++,
		       (int)got[SIGUSR1], (int)got[SIGRTMIN]);
		fflush(stdout);
		sleep(1);
	}
	return 0;
}
```

**五种不同的 disposition 在一个进程里**(自定义 handler + `SA_SIGINFO` +
非空 `sa_mask` + `SIG_IGN` + `SIG_DFL` + 非空 blocked mask),因为一个「只存
handler 地址,不存 flags 和 mask」的实现能通过只检查 handler 的测试。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 4 个自定义 handler | 恢复后 `sigaction` 读回的地址一致 |
| 2 | `sa_flags` 含 `SA_SIGINFO`/`SA_RESTART` | flags 一致 |
| 3 | 非空 `sa_mask` | mask 一致 |
| 4 | `SIG_IGN` 的信号 | 仍然是 `SIG_IGN`,不是默认 |
| 5 | `SIG_DFL` 的信号 | 仍然是默认 |
| 6 | blocked mask 非空 | `/proc/$PID/status` 的 `SigBlk` 一致 |
| 7 | pending 的标准信号(被屏蔽着) | 解除屏蔽后被投递,**恰好一次** |
| 8 | pending 的实时信号 × 5(不合并) | 解除屏蔽后收到**恰好 5 个**,顺序一致 |
| 9 | pending 信号的 `siginfo` 内容 | `si_value` / `si_pid` 一致 |
| 10 | 多线程:每线程不同 blocked mask | 每线程各自的 `SigBlk` 一致 |
| 11 | 多线程:pending 在共享队列 | 只被一个线程收到,不是每线程一次 |
| 12 | 多线程:pending 在特定线程队列(`tgkill`) | 被那个线程收到 |
| 13 | POSIX 定时器,剩余 30 秒 | 恢复后约 30 秒后触发(±2 秒) |
| 14 | POSIX 定时器,已到期未投递 | 恢复后立即触发 |
| 15 | 周期性 POSIX 定时器 | 周期保持 |
| 16 | `SIGEV_THREAD_ID` 定时器 | 信号发给正确的线程 |
| 17 | `ITIMER_REAL` 剩余时间 | 一致(±2 秒) |
| 18 | `SIGKILL`/`SIGSTOP` 的表项 | 镜像里 64 项齐全,restore 不报错 |

用例 8 是这一步最有力的测试。实时信号不合并,所以「恰好 5 个、顺序一致」是一个
非常强的断言 —— 它同时验证了 pending 队列的完整性、顺序、以及 siginfo 的正确性。
标准信号会合并,用它测不出这些。

用例 11 和 12 是 A4 里 4.2 那个「两个队列」的验收。**它们互为反向**:11 抓
「把共享队列当成每线程队列存了」(信号被投递 N 次),12 抓「把每线程队列
合并进共享队列了」(信号被错误的线程收到)。

用例 13 的 ±2 秒容差是给 TCG 模式下的 CI 留的余量。

### 5.3 定时器测试的时间验证

```sh
# Timer semantics: CRIU stores time REMAINING, so a restore hours later must
# still wait the remaining interval -- not fire instantly, and not restart
# the full original interval.
./tests/progs/timers 30 > /tmp/t.out &   # 30-second timer
sleep 5                                   # 25s remaining
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/t.out)

insmod kernel_module/criu_kernel.ko
echo "$PID /tmp/timg" > /sys/kernel/debug/criu/dump
rmmod criu_kernel
kill -9 $PID

sleep 20                                  # deliberately wait past the original
                                          # absolute deadline
T0=$(date +%s)
criu restore -D /tmp/timg --restore-detached >> /tmp/t.out 2>&1
# Must fire ~25s after restore, NOT immediately (which would mean we stored
# an absolute deadline) and NOT 30s (which would mean we reset the interval).
while ! grep -q 'TIMER FIRED' /tmp/t.out; do
	sleep 1
	[ $(( $(date +%s) - T0 )) -gt 40 ] && { echo "FAIL: timer never fired"; exit 1; }
done
ELAPSED=$(( $(date +%s) - T0 ))
[ "$ELAPSED" -ge 22 ] && [ "$ELAPSED" -le 28 ] || {
	echo "FAIL: fired after ${ELAPSED}s, expected ~25s"; exit 1; }
echo "timer semantics OK"
```

**这个测试的设计是本步骤的重点。** `sleep 20` 是刻意的:它让原始的绝对到期时刻
在 restore 之前就过去了。于是:

- 存了绝对时刻的实现 → 立即触发 → `ELAPSED` 约 0 → 失败
- 重置了间隔的实现 → 30 秒后触发 → `ELAPSED` 约 30 → 失败
- 正确实现 → 25 秒后触发 → 通过

三种结果分得干净,没有任何一种错误实现能碰巧通过。

### 5.4 ZDTM 增量

```
zdtm/static/sigaltstack
zdtm/static/sigpending
zdtm/static/pending_signal
zdtm/static/posix_timers
zdtm/static/timers
zdtm/static/sigaction_bug
```

---

## 6. 完成标准

- [ ] 18 个用例通过,含 8、11、12 三个反向配对用例
- [ ] 定时器时间语义测试通过(三种结果可区分)
- [ ] 所有持 `siglock` 的代码路径经 `DEBUG_ATOMIC_SLEEP` 验证无睡眠
- [ ] `sa_restorer` 原样保存,未重算
- [ ] A3/A4/A5 测试全部仍通过
- [ ] allowlist 增加至少 4 个测试
