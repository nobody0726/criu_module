# A2 —— 冻结 / 解冻

**工期:** 1 周 · **前置:** A1 · **产出:** 可靠冻结多线程进程,无信号丢失

> 相关原理:[02-freezing](../principles/02-freezing.md)

---

## 1. 设计思路

### 为什么冻结要单独成一步

它是 dump 侧**唯一的强前置依赖** —— 所有资源收集都必须在冻结之后。同时它有一个
很好的性质:**单独就能完整测试**。冻结一个进程 → 确认它真停了 → 解冻 → 确认它
正常继续。不需要任何 dump 能力。

一步一个强依赖,而且这个依赖自带测试方法,这是理想的步骤形状。

### 冻结要保证什么

三条,按重要性排:

1. **地址空间不再变化。** 否则 A1 的读取层会读到撕裂状态。
2. **寄存器状态已经落到内存里。** 一个正在 CPU 上跑的任务,它的寄存器在 CPU 里,
   不在 `task->thread`。必须让它彻底停下,内核才会把寄存器保存到内核栈上。
3. **不能被目标进程观测到。** 这是最微妙的一条,见下节。

### 「不能被观测到」为什么排除了 SIGSTOP

直觉方案是 `kill(pid, SIGSTOP)`。它有三个致命问题:

**问题 1:目标能看见。** `SIGSTOP`/`SIGCONT` 会改变父进程 `wait()` 的返回值
(`WIFSTOPPED`)。如果目标进程的父亲正在 `waitpid()`,它会观测到一次「孩子停了又
继续了」的事件 —— 这个事件在原始时间线里不存在。checkpoint 的整个前提是
「进程分辨不出发生过 checkpoint」,这里就破了。

**问题 2:`SIGCONT` 会清掉 pending 的 `SIGSTOP`,反之亦然。** 如果目标进程在
checkpoint 之前**自己**收到了一个真的 `SIGSTOP`(还没处理),你的 `SIGCONT` 会
把它吃掉。你恢复了一个状态错误的进程。

**问题 3:停止状态本身是要被 dump 的状态。** 目标进程可能**本来就是 stopped 的**
(比如被调试器停着,或者收到过 `SIGTSTP`)。你用 `SIGSTOP` 冻结它,就没法区分
「本来停的」和「被我停的」,解冻时也就不知道该不该恢复成停止态。

### 所以用 PTRACE_SEIZE

`PTRACE_SEIZE` 是 Linux 3.4 引入的,专门解决上述问题。它和老的 `PTRACE_ATTACH`
的区别:

| | `PTRACE_ATTACH` | `PTRACE_SEIZE` |
|---|---|---|
| 附加时是否发信号 | 发 `SIGSTOP` | **不发任何信号** |
| 是否影响 `wait()` 语义 | 是 | 否 |
| 能否区分 group-stop 和 signal-delivery-stop | 不能 | 能(靠 `PTRACE_LISTEN`) |
| 分离时目标状态 | 可能残留 stopped | 干净恢复 |

配合 `PTRACE_INTERRUPT` 让目标进 ptrace-stop,配合 `PTRACE_LISTEN` 在目标本来就
是 group-stop 时保持它的 group-stop 状态。**这套组合是唯一能满足「不可观测」的
方案。**

### 内核模块里怎么做

这里有个岔路。内核模块**不能调 `ptrace()` 系统调用**(那是给用户空间的),但它可以:

- **方案 A:** 直接操作 `task->jobctl` 和 `signal->group_stop_count`,复现
  `ptrace_attach` 的效果。**不推荐** —— 这些字段的一致性约束极复杂,`freezer`
  和 `signal` 子系统会打架。
- **方案 B:** 用 cgroup freezer。给目标进程建一个 freezer cgroup,写 `FROZEN`。
- **方案 C:** 用 `kernel_signal_stop()` / `freeze_task()` 这类内核内部接口。

**推荐方案 B。** 理由:

1. cgroup freezer 就是内核为「批量冻结一组任务」设计的,语义正确、经过检验
2. 它天然处理多线程和进程树 —— 整个 cgroup 一起冻
3. 它冻结的是**调度层面**,不动信号状态,天然满足「不可观测」
4. 通过 `cgroup_attach_task()` / kernfs 写入操作,不需要未导出符号

代价:需要 `CONFIG_CGROUP_FREEZER`(`build-kernel.sh` 已开),而且目标进程会被
临时移进一个新 cgroup —— **这本身就是可观测的**(`/proc/PID/cgroup` 变了)。
所以 A2 必须做的一件事是:**记下原 cgroup 路径,解冻后移回去。**

这是本步骤最容易漏的点,而且漏了之后症状很隐蔽(进程能跑,但 cgroup 归属错了,
资源限制不生效)。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 主文件

`criu/criu/seize.c`。核心函数 `seize_catch_task()` / `seize_wait_task()` /
`compel_interrupt_task()`。

### 2.2 CRIU 也用 cgroup freezer —— 但只在给了 `--freeze-cgroup` 时

`criu/criu/seize.c` 里有两条路:

- 默认:逐个 task `PTRACE_SEIZE`
- `--freeze-cgroup`:先用 cgroup freezer 冻整组,再 seize

第二条存在的理由很重要:**纯 ptrace 方案有竞态。** `criu/criu/seize.c:973`
附近的注释点明了:

> new ones can appear (with clone(CLONE_PARENT) or with ...)

也就是说,你在遍历进程树逐个 seize 的过程中,**已经 seize 的进程可能 fork 出新的
孩子**,而新孩子没被 seize。CRIU 的应对是反复扫描 `/proc` 直到两次扫描结果一致
(收敛),但这在进程频繁 fork 时可能长时间不收敛。

`--freeze-cgroup` 一次性冻住整个 cgroup,**从根上消灭了这个竞态**。

**这是内核模块的一个真实优势:我们可以把 cgroup freezer 作为默认而非可选路径,
因为我们不像 CRIU 那样需要 ptrace 来注入 parasite。** CRIU 即使用了 freezer,
之后还是得 seize,因为它需要 ptrace 通道来注入代码。我们不需要。

### 2.3 CRIU 怎么区分「本来就停着」

`criu/criu/seize.c` 里判断 `/proc/PID/stat` 的第三个字段(state)是不是 `T`,
并且用 `PTRACE_LISTEN` 保持 group-stop。恢复时 `core.proto` 里有相应字段记录
任务原本的状态。

**要抄的:** 区分「本来停着」vs「被我停的」这个必要性;收敛扫描的思路作为
freezer 的兜底校验。
**不抄的:** ptrace seize 那一整套 —— 我们用 freezer 就不需要了。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/freeze.c`
- `tests/progs/busy-counter.c`
- `tests/progs/multithread-counter.c`
- `tests/progs/fork-bomb-lite.c`
- `tests/compare/freeze-test.sh`

**Modify:**
- `kernel_module/include/criu_kernel.h` —— 加下面的接口
- `kernel_module/core/main.c` —— 注册 `freeze` / `thaw` debugfs 入口

**Interfaces produced:**

```c
/* Opaque handle describing a frozen task group, including the information
 * needed to put it back exactly where it came from. */
struct criu_freeze_ctx;

/* Freeze the whole thread group of vpid, plus (optionally) descendants.
 * On success *ctx receives a handle that must be passed to criu_thaw().
 * Returns 0, or -ESRCH / -EPERM / -ETIMEDOUT. */
int criu_freeze(pid_t vpid, bool include_children,
		struct criu_freeze_ctx **ctx);

/* Reverse criu_freeze(): thaw, restore the original cgroup membership, and
 * free the context. Safe to call with ctx == NULL. */
void criu_thaw(struct criu_freeze_ctx *ctx);

/* True once every task in the group is off-CPU and its registers have been
 * saved to its kernel stack -- i.e. task_pt_regs() is meaningful. */
bool criu_freeze_settled(struct criu_freeze_ctx *ctx);
```

`criu_freeze_settled()` 单独成一个接口,是因为「cgroup 说 FROZEN」和「所有线程真
的下了 CPU」**不是同一件事**。见 4.2。

---

## 4. 关键实现要点

### 4.1 记住原 cgroup 并恢复

```c
struct criu_freeze_ctx {
	struct cgroup *orig_cgrp;	/* where the task lived before us */
	struct cgroup *freeze_cgrp;	/* our temporary freezer cgroup */
	struct task_struct **tasks;	/* pinned refs, so nobody vanishes */
	int nr_tasks;
	bool was_stopped;		/* target was already in TASK_STOPPED */
};
```

`was_stopped` 必须在**冻结之前**采样。冻结之后再读就分不清了。

### 4.2 「FROZEN」不等于「寄存器已落盘」

cgroup freezer 把任务标记成冻结后,任务会在下一次进入内核态时停下。但一个正在
用户态 spin 的任务,可能要等到下一次时钟中断才被处理。在那之前
`task_pt_regs(task)` 里的内容是**上一次进内核时的**,不是当前的。

判定条件:

```c
bool criu_freeze_settled(struct criu_freeze_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->nr_tasks; i++) {
		struct task_struct *t = ctx->tasks[i];

		/* task_is_running() means it is on a CPU or runnable, so its
		 * registers may still be live in hardware rather than saved
		 * on the kernel stack.
		 */
		if (task_is_running(t))
			return false;
	}
	return true;
}
```

调用者要**轮询加超时**,不能假设一次就成:

```c
	for (i = 0; i < CRIU_FREEZE_POLL_MAX; i++) {
		if (criu_freeze_settled(ctx))
			break;
		msleep(10);
	}
	if (i == CRIU_FREEZE_POLL_MAX)
		return -ETIMEDOUT;
```

**这个超时路径必须测。** 一个死循环里从不进内核的用户态线程(纯计算,不调
syscall)是真实存在的,时钟中断会救你,但如果内核配了 `CONFIG_NO_HZ_FULL` 且
该 CPU 是 nohz_full 的,就可能真的很久不响应。测试用例 7 覆盖它。

### 4.3 `task_pt_regs` 是宏,不是函数

`linux-5.10.29/arch/x86/include/asm/processor.h:763` 定义 `task_pt_regs` 为宏。
所以它**不需要导出**,直接能用 —— 这是个好消息。但它返回的指针指向内核栈顶,
只在任务停止时有效。

### 4.4 收敛校验(借 CRIU 的思路兜底)

即使用了 freezer,也要做一次校验:冻结后遍历一遍目标进程树,确认没有 task 处于
`TASK_RUNNING`,且**task 数量两次扫描一致**。如果不一致,说明有任务逃出了
cgroup(理论上不该发生,但 `clone(CLONE_PARENT)` 加上 cgroup 迁移的竞态窗口值得
一条断言)。

---

## 5. 如何测试

### 5.1 核心思路:用「计数器不前进」证明真的停了

```c
/* busy-counter.c: writes a monotonically increasing counter to a shared file
 * so an external observer can prove whether the process is making progress. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
	unsigned long *counter;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <shared-file>\n", argv[0]);
		return 1;
	}

	fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return 1;
	if (ftruncate(fd, sizeof(*counter)))
		return 1;

	/* MAP_SHARED so the observer reads our progress without any syscall
	 * on our side -- a syscall would itself be a freeze checkpoint and
	 * would muddy what we are measuring.
	 */
	counter = mmap(NULL, sizeof(*counter), PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (counter == MAP_FAILED)
		return 1;

	printf("pid=%d\n", getpid());
	fflush(stdout);

	for (;;)
		(*counter)++;

	return 0;
}
```

用 `MAP_SHARED` 文件而不是 `printf` 是刻意的:**`printf` 会调 `write()`,而系统
调用本身就是一个进内核的点,会掩盖「纯用户态 spin 时能不能冻住」这个问题。**
这个测试程序的设计本身就是在测 4.2 那个坑。

### 5.2 测试脚本

```sh
#!/bin/sh
# A2 acceptance: prove the target really stops, really resumes, and cannot
# tell that it happened.
set -e

CNT=/tmp/a2-counter
./tests/progs/busy-counter "$CNT" > /tmp/a2.out &
sleep 1
PID=$(sed -n 's/^pid=\([0-9]*\)/\1/p' /tmp/a2.out)

read_counter() { od -An -tu8 -N8 "$CNT" | tr -d ' '; }

insmod kernel_module/criu_kernel.ko

# --- running: the counter must advance ---
a=$(read_counter); sleep 1; b=$(read_counter)
[ "$b" -gt "$a" ] || { echo "FAIL: counter not advancing before freeze"; exit 1; }

# --- frozen: the counter must NOT advance ---
echo "$PID" > /sys/kernel/debug/criu/freeze
c=$(read_counter); sleep 2; d=$(read_counter)
[ "$c" = "$d" ] || { echo "FAIL: counter advanced while frozen ($c -> $d)"; exit 1; }

# --- registers must be readable while frozen ---
grep -q '^rip=' /sys/kernel/debug/criu/regs \
	|| { echo "FAIL: regs unavailable while frozen"; exit 1; }

# --- cgroup membership must be preserved across freeze/thaw ---
before=$(cat /proc/$PID/cgroup)
echo "$PID" > /sys/kernel/debug/criu/thaw
after=$(cat /proc/$PID/cgroup)
[ "$before" = "$after" ] || {
	echo "FAIL: cgroup changed: [$before] -> [$after]"; exit 1; }

# --- thawed: the counter must advance again ---
e=$(read_counter); sleep 1; f=$(read_counter)
[ "$f" -gt "$e" ] || { echo "FAIL: counter did not resume"; exit 1; }

kill $PID 2>/dev/null || true
rmmod criu_kernel
echo "A2 OK"
```

### 5.3 必须覆盖的测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 单线程冻结/解冻 | 计数器停止再恢复 |
| 2 | 多线程(8 线程)冻结 | **每个**线程的计数器都停 |
| 3 | 冻结期间寄存器可读 | `task_pt_regs` 有意义,`rip` 落在代码段内 |
| 4 | cgroup 归属恢复 | `/proc/PID/cgroup` 前后一致 |
| 5 | 目标**本来就是 stopped** | 解冻后**仍然是** stopped(不能被叫醒) |
| 6 | 冻结期间发一个 `SIGUSR1` | 解冻后信号被正常投递,**一个不丢** |
| 7 | 纯用户态 spin 不调 syscall 的线程 | 能冻住,或明确 `-ETIMEDOUT` |
| 8 | 冻结期间目标 `kill -9` | `criu_thaw` 不 oops |
| 9 | 冻结进程树(父 + 3 层子孙) | 全部停止 |
| 10 | 冻结过程中有进程在 fork | 收敛校验通过,或明确报错 |
| 11 | 双重冻结(freeze 两次) | 第二次返回 `-EBUSY`,不破坏状态 |
| 12 | `rmmod` 时还有进程处于冻结态 | 拒绝卸载(`-EBUSY`),或先自动 thaw |

用例 5 和 6 是**这一步的灵魂**。它们检验的是「不可观测」这条,而这条是冻结方案
选择的全部理由。

用例 6 的构造:目标程序装一个 `SIGUSR1` handler 累加第二个计数器。冻结 → 发 10 个
`SIGUSR1` → 解冻 → 检查第二个计数器**恰好**增加了应有的数量。注意标准信号会合并,
所以要么发 1 个,要么用实时信号 `SIGRTMIN`(不合并)。**用 `SIGRTMIN` 并发 10 个,
检查恰好收到 10 个 —— 这才是有力的测试。**

用例 12 是资源管理:模块卸载时把进程留在冻结态,那些进程就永久卡死了,只能重启。

### 5.4 加入 CI

`tests/ci-smoke.sh` 增加:

```sh
sh tests/compare/freeze-test.sh || exit 1
```

注意 CI 在 TCG 模式下慢 10 倍,`sleep 1` 可能不够计数器跑出可测的增量。脚本里的
比较用 `-gt` 而不是「增量大于某个阈值」,正是为了在慢环境下也稳定。

---

## 6. 完成标准

- [ ] 12 个用例全部通过,含 5/6/7 三个难点
- [ ] dmesg 干净(`DEBUG_ATOMIC_SLEEP` 尤其重要 —— freezer 路径里容易误睡眠)
- [ ] `criu_freeze` / `criu_thaw` / `criu_freeze_settled` 接口冻结
- [ ] 超时路径有测试覆盖,不是只在代码里存在
- [ ] `freeze-test.sh` 进 CI,绿
