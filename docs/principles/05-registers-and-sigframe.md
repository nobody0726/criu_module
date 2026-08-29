# 原理 05 —— 寄存器,以及 rt_sigreturn 这个魔法

> 被引用于:[A3](../steps/A3-minimal-dump.md)、[A4](../steps/A4-threads.md)、
> [A6](../steps/A6-signals-timers.md)、[B1](../steps/B1-mini-restore.md)

**本篇是全项目唯一一篇架构相关的原理。** 其余 8 篇在 x86_64 和 aarch64 上一字不差。

---

## 0. 为什么这个项目是 aarch64

先说清楚哪些东西真的绑架构,因为这决定了「换架构」的代价有多大:

| 绑架构 | 与架构无关 |
|---|---|
| 寄存器名与数量 | VMA 遍历、`vm_next` |
| `rt_sigframe` 布局 | cgroup freezer |
| `ARCH_RT_SIGRETURN` 那几条汇编 | fd / pipe / shmem |
| `core-$arch.proto` 字段号 | 镜像格式与 protobuf 编码 |
| `task_pt_regs` 的定义 | pid / session / pgid |
| 浮点状态的格式 | `clone3(set_tid)`(`__NR_clone3 = 435` 来自 `asm-generic/unistd.h`) |
| TLS 寄存器 | 内核模块的符号限制 |

**左列基本就是这一篇的内容。** 所以换架构要改的是这一篇 + A3/A4 的寄存器段 +
B1 的最后一跳,不是整个计划。

选 aarch64 的三个理由:

1. **开发机是 Apple Silicon。** 原生速度,不用 TCG 模拟。这是便利,不是理由的核心。
2. **aarch64 的寄存器/浮点模型干净得多。** 具体对比:

   | | x86_64 | aarch64 |
   |---|---|---|
   | 浮点格式 | `xsave`:512B FXSAVE 区 + 64B header + 变长扩展区,`xstate_bv` 位图必须与内容一致,总大小靠 `CPUID` 算 | `vregs[32]` + `fpsr` + `fpcr`,定长 |
   | CRIU 的 FPU 准备函数 | `criu/arch/x86/sigframe.c` 里一整套 | `criu/arch/aarch64/sigframe.c` 的 `sigreturn_prep_fpu_frame()` 是 `return 0;` |
   | 32 位兼容层 | 有,`ARCH_RT_SIGRETURN_COMPAT` 是另一条完整分支 | 无。`kdat_compatible_cr()` 硬编码为 `0` |
   | 段寄存器 | `cs`/`ss`/`ds`/`es`/`fs`/`gs` + `fs_base`/`gs_base` | 没有段的概念,TLS 是一个系统寄存器 |

   x86 那套复杂度是历史包袱,不是知识。**对一个以「学清楚机制」为目标的项目,
   少一层包袱是实质收益。**

3. **ZDTM oracle 几乎没有损失。** 489 个静态测试里只有 13 个带 `arch` 键
   (`test/zdtm.py:2730` 的逻辑是 `tdesc.get('arch', arch) != arch` —— 没有
   `arch` 键的测试在所有架构上都跑)。x86_64 独占的只有 9 个:`fpu00`–`fpu03`、
   `mmx00`、`sse00`、`sse20`、`rseq00`、`vdso01`,全是 FPU/SIMD/vDSO 类。
   而上游 CRIU 的 `aarch64-test`(`.github/workflows/ci.yml:59`)在真 arm64
   机器上跑全套 ZDTM,只排除 2 个测试。**这个 oracle 是被上游持续验证的。**

### 一个必须先付的代价:PAC

`criu/criu/arch/aarch64/crtools.c:117` 无条件用 `NT_ARM_PAC_ENABLED_KEYS`
调 `PTRACE_GETREGSET`,而那个 regset 在 5.10.29 里不存在。所以本项目要求
`CONFIG_ARM64_PTR_AUTH=n`,细节和实验门禁见
[04-Dev-Environment 第 3.1 节](../04-Dev-Environment.md)。

**代价是不覆盖 PAC 密钥的 C/R,写进限制列表。** 这是本项目在 scope 上反复使用的
那个判据:明确不支持优于静默错误。

---

## 1. 一个线程的「执行位置」是什么

一个正在运行的线程,它的「当前位置」完全由 CPU 寄存器决定:

| 寄存器 | 含义 |
|---|---|
| `pc` | 下一条要执行的指令地址(x86 的 `rip`) |
| `sp` | 栈顶(x86 的 `rsp`) |
| `regs[0]`..`regs[30]` | 31 个通用寄存器 x0–x30 |
| `regs[29]` | 习惯上是帧指针 fp |
| `regs[30]` | 链接寄存器 lr —— **函数返回地址在寄存器里,不在栈上** |
| `pstate` | 处理器状态(条件标志 NZCV、中断屏蔽等),x86 的 `eflags` |
| `tpidr_el0` | TLS 基址 —— **`__thread` 变量就靠它** |
| `vregs[32]` + `fpsr` + `fpcr` | 浮点/SIMD 状态,定长 |

**这些加起来就是「执行状态」的全部。** 存下来,恢复时写回去,线程就从原来那条
指令继续执行。

这是纯 A 类状态 —— 就是一堆字节,没有任何需要向内核重建的语义。

`lr` 值得单独注意:aarch64 的函数返回地址在 `regs[30]` 里,而不是像 x86 那样
压在栈上。它是 31 个通用寄存器之一,所以**只要 `regs[]` 全部存对就自动正确**,
不需要特殊处理。提出来只是因为它容易让读惯 x86 的人困惑。

### `core-aarch64.proto` 的字段

```protobuf
message user_aarch64_regs_entry {
	repeated uint64 regs	= 1;
	required uint64 sp	= 2;
	required uint64 pc	= 3;
	required uint64 pstate	= 4;
}
```

**四个字段。** 对比 `core-x86.proto` 的 27 个。

`regs` 是 `repeated`,不是固定 31 个字段 —— 所以序列化时要按顺序 append 31 次,
**顺序就是语义**,错一位就是 x5 的值跑到 x6 里。这是 protobuf `repeated` 字段
的通用风险,[04-image-format](04-image-format.md) 里讲过。

---

## 2. 从内核里读寄存器

### `task_pt_regs`

一个被停下来的任务,它的用户态寄存器保存在**内核栈的顶端**。内核在从用户态进入
内核态时(系统调用、中断、异常)会把它们压在那里。

```c
#define task_pt_regs(p) \
	((struct pt_regs *)(THREAD_SIZE + task_stack_page(p)) - 1)
```

(`arch/arm64/include/asm/processor.h:257`)

就是「内核栈顶往下退一个 `pt_regs` 的大小」。比 x86 那版还短一点 —— x86 要减一个
`TOP_OF_KERNEL_STACK_PADDING`,arm64 没有这个补丁。

**它是一个宏,所以不需要任何导出符号。** 对内核模块来说这很重要 ——
`EXPORT_SYMBOL` 的限制在这里完全不适用。

对比 CRIU:它必须对**每个线程**单独 `PTRACE_ATTACH` + `PTRACE_GETREGSET`。
内核模块 `task_pt_regs(t)` 对任意线程直接可用。**这是内核模块的第二个真实优势**
(第一个是不需要 parasite)。

### 前提:任务必须真的停下来了

`task_pt_regs()` 只在任务被调度出去、上下文完整保存之后才可靠。如果任务还在 CPU
上跑,那块内存里是上一次进入内核时的旧值。

这就是 [02-freezing](02-freezing.md) 里强调「FROZEN ≠ 寄存器可读」的原因:
写完 `FROZEN` 必须轮询 `task_is_running()` 直到所有线程都真的停了。

### aarch64 的一个便利:ptrace ABI 结构体是内嵌的

x86 上 `struct pt_regs` 和 `struct user_regs_struct` 是两个独立定义、字段顺序
不同的结构体,所以**不能 `memcpy`**。arm64 不一样:

```c
struct pt_regs {
	union {
		struct user_pt_regs user_regs;
		struct {
			u64 regs[31];
			u64 sp;
			u64 pc;
			u64 pstate;
		};
	};
	u64 orig_x0;
	s32 syscallno;
	/* ... 其余内核内部字段 ... */
};
```

(`arch/arm64/include/asm/ptrace.h`)

**ptrace ABI 的 `struct user_pt_regs` 就是 `pt_regs` 的第一个成员**,内核自己用
union 保证了两者布局一致。所以 `&task_pt_regs(t)->user_regs` 直接就是 ABI 视图。

即便如此,**填 protobuf 时仍然按字段名逐个赋值**,不要指望结构体和 proto 消息
有任何布局关系 —— proto 是变长编码,和 C 结构体没有对应。CRIU 自己也是逐个赋:

```c
	for (i = 0; i < 31; ++i)
		assign_reg(core->ti_aarch64->gpregs, regs, regs[i]);
	assign_reg(core->ti_aarch64->gpregs, regs, sp);
	assign_reg(core->ti_aarch64->gpregs, regs, pc);
	assign_reg(core->ti_aarch64->gpregs, regs, pstate);
```

(`criu/criu/arch/aarch64/crtools.c` 的 `save_task_regs()`)

注意 `orig_x0` 和 `syscallno` **在 union 之外** —— 它们不属于 ptrace ABI,
用户态看不到,proto 里也没有。这一点是下一节的关键。

### 内核模块在这里多拿两样东西

`tpidr_el0`(TLS)和浮点状态都**不在** `pt_regs` 里,它们在 `thread_struct`:

```c
	/* Both TLS and FPSIMD live in thread.uw, not in pt_regs. A module reads
	 * them straight out of the task; CRIU needs a parasite for the former
	 * and a ptrace regset for the latter.
	 */
	u64 tls = task->thread.uw.tp_value;
	struct user_fpsimd_state *fp = &task->thread.uw.fpsimd_state;
```

CRIU 拿 TLS 的方式是在**parasite 里**执行一条指令:

```c
static inline void arch_get_tls(tls_t *ptls)
{
	tls_t tls;
	asm("mrs %0, tpidr_el0" : "=r"(tls));
	*ptls = tls;
}
```

(`criu/criu/arch/aarch64/include/asm/parasite.h:4-8`)

`mrs tpidr_el0` 只能读**自己的**,所以必须让目标进程自己执行 —— 这就是为什么这件事
非得走 parasite。内核模块直接读 `thread.uw.tp_value`,**这是第五个优势**,而且它是
aarch64 特有的:x86 上 CRIU 有 `ptrace(PTRACE_ARCH_PRCTL)` 可以旁路,不需要 parasite。

`uw` 这个子结构的存在本身也是一个信号 —— 内核把它单独分出来,是为了给
hardened usercopy 划白名单(见 `arch/arm64/kernel/process.c` 里的
`arch_task_struct_size` 相关注释)。里面装的正是「可以给用户态看」的那部分。

---

## 3. 系统调用重入:x86 的 `orig_ax` 在 aarch64 去哪了

这是 x86 → aarch64 变化最大的一节,而且方向是**变简单**。

### 问题本身在两个架构上都存在

进程被冻结时,它很可能正停在一个阻塞系统调用里 —— `read()`、`pause()`、
`epoll_wait()`。恢复之后,它必须**重新进入**那个系统调用,而不是拿着一个垃圾返回值
往下走。

x86 用 `orig_ax` 解决:内核在进入系统调用时把系统调用号存进 `orig_ax`,
`rax` 则被返回值覆盖。CRIU 检查 `orig_ax` 来判断「是否停在系统调用里」,
并把 `rip` 回退 2 字节(`syscall` 指令的长度)。`core-x86.proto` 里有 `orig_ax`
这个字段就是为此。

### aarch64:内核已经替你做完了

`struct user_pt_regs` 里**没有** `syscallno`,也没有 `orig_x0`,所以
`core-aarch64.proto` 里没有对应字段。这不是遗漏,是因为不需要:

```c
	if (syscall) {
		continue_addr = regs->pc;
		restart_addr = continue_addr - (compat_thumb_mode(regs) ? 2 : 4);
		retval = regs->regs[0];

		/*
		 * Avoid additional syscall restarting via ret_to_user.
		 */
		forget_syscall(regs);

		/*
		 * Prepare for system call restart. We do this here so that a
		 * debugger will see the already changed PC.
		 */
		switch (retval) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
		case -ERESTART_RESTARTBLOCK:
			regs->regs[0] = regs->orig_x0;
			regs->pc = restart_addr;
			break;
		}
	}
```

(`arch/arm64/kernel/signal.c:851-876`,`do_signal()`)

逐句读这段:

1. `restart_addr = pc - 4` —— aarch64 指令定长 4 字节,`svc #0` 就在 `pc - 4`。
2. `forget_syscall(regs)` 把 `syscallno` 置为 `NO_SYSCALL`
   (`arch/arm64/include/asm/ptrace.h:207-210`)。
3. 如果返回值是 `-ERESTART*`,**把 `regs[0]` 恢复成原始的第一个参数,并把 `pc`
   退回 `svc` 那条指令上。**

第 3 步就是 CRIU 在 x86 上要手动做的事。而那句注释是这一节的关键证据:

> **so that a debugger will see the already changed PC**

**内核显式承诺:调试器看到的是已经改好的 PC。** 所以任何通过信号路径观察到的
寄存器快照,已经是「可重入形式」了。存下来直接用,不需要额外字段。

### 这条保证覆盖我们的两条采集路径吗

要,而且都覆盖 —— 关键在于 `do_signal()` 的重入调整在 `get_signal()` **之前**:

| 采集路径 | 停在哪 | 是否经过 `do_signal()` |
|---|---|---|
| CRIU:`PTRACE_SEIZE` + `PTRACE_INTERRUPT` | ptrace-stop | 是。ptrace 停点在 `get_signal()` 里 |
| 本项目:cgroup freezer | freezer trap | 是。`kernel/signal.c:2625` 的 `do_freezer_trap()` 也在 `get_signal()` 里 |

两条路都是「先调整 `pc`,再停下来」。**所以 A2 的冻结路径继承了同一个保证**,
这也是本节值得完整读源码的原因 —— 结论不是「CRIU 这么做所以我们这么做」,而是
「内核在这个位置做了这件事,而我们的采集点在它之后」。

> **这一节的结论是读源码得出的,还没有实测。** 具体待验:一个阻塞在 `pause()` 里
> 的进程,冻结后 `task_pt_regs()` 里的 `pc` 是否确实指向 `svc` 指令(而不是它的
> 下一条)。**这是 A3 欠的一个测试用例**,列在 A3 的测试表里。
> 判定方法很直接:读出 `pc`,再从 `/proc/PID/mem` 的 `pc` 处读 4 字节,
> 检查它是不是一条 `svc #0`(编码 `0xd4000001`)。

---

## 4. 浮点状态:定长,但有一个字段序陷阱

### 格式本身很简单

```c
struct user_fpsimd_state {
	__uint128_t	vregs[32];
	__u32		fpsr;
	__u32		fpcr;
	__u32		__reserved[2];
};
```

(`arch/arm64/include/uapi/asm/ptrace.h`)

**32 个 128 位向量寄存器 + 两个 32 位控制/状态寄存器,定长 528 字节。** 没有位图,
没有 `CPUID` 计算,没有变长扩展区。CRIU 在 aarch64 上的 FPU 准备函数是:

```c
int sigreturn_prep_fpu_frame(struct thread_restore_args *args,
			     struct thread_restore_args *ret_args)
{
	return 0;
}
```

(`criu/criu/arch/aarch64/sigframe.c`)

对比 x86 那边一整套 `xsave` 大小探测、`xstate_bv` 位图校验、对齐处理。
**这个 `return 0;` 是选 aarch64 最直观的收益证据。**

### 冻结的任务的 FPSIMD 是可读的,但 SVE 任务需要同步

```c
static int __fpr_get(struct task_struct *target,
		     const struct user_regset *regset,
		     struct membuf to)
{
	struct user_fpsimd_state *uregs;

	sve_sync_to_fpsimd(target);

	uregs = &target->thread.uw.fpsimd_state;

	return membuf_write(to, uregs, sizeof(*uregs));
}
```

(`arch/arm64/kernel/ptrace.c:598-609`)

两点要看:

1. **`fpsimd_preserve_current_state()` 只在 `target == current` 时被调用**
   (在外层的 `fpr_get` 里)。对一个被冻结的**别的**任务,它的 FPSIMD 在被调度出去时
   就已经落到 `thread.uw.fpsimd_state` 里了,直接读即可。
2. **但 `sve_sync_to_fpsimd(target)` 必须照做。** 如果任务在用 SVE,寄存器的权威副本
   在 `thread.sve_state` 里,`uw.fpsimd_state` 是过期的。这个函数把前者的低 128 位
   同步回后者。

`sve_sync_to_fpsimd` 是否导出、能否在模块里调,**是 S0 要验的一项**。
拿不到的退路是明确检测 `thread.sve_state != NULL` 并返回 `-EOPNOTSUPP` ——
按本项目的判据,明确不支持优于静默读到过期数据。

> 注意这个退路的性质:**它比「不支持」更强,它是「检测到就报错」。**
> 一个只读 `uw.fpsimd_state` 而不检查 SVE 的实现,在 SVE 进程上会**安静地**
> 存下错误的向量寄存器,恢复后的浮点计算结果错。这是本文件里第二个「症状是
> 静默错误」的地方。

### 陷阱:ptrace 结构体和 sigframe 结构体的字段顺序是反的

这是 aarch64 上唯一一个能造成静默数据损坏的新问题,值得单独记住:

```c
/* ptrace / thread_struct 视图 */
struct user_fpsimd_state {
	__uint128_t	vregs[32];	/* 先 */
	__u32		fpsr;		/* 后 */
	__u32		fpcr;
	__u32		__reserved[2];
};

/* sigframe 视图 */
struct fpsimd_context {
	struct _aarch64_ctx head;	/* 多一个头 */
	__u32		fpsr;		/* 先 */
	__u32		fpcr;
	__uint128_t	vregs[32];	/* 后 */
};
```

(前者 `arch/arm64/include/uapi/asm/ptrace.h`,后者 `arch/arm64/include/uapi/asm/sigcontext.h`)

**两者字段完全相同,顺序完全相反,还差一个 `head`。** 所以:

```c
	/* NEVER memcpy between user_fpsimd_state and fpsimd_context: same
	 * fields, reversed order, plus a _aarch64_ctx header. Copy field by
	 * field. A memcpy compiles, runs, and silently produces garbage
	 * vregs.
	 */
```

A3(dump 侧,写 `core-aarch64.proto`)和 B1(restore 侧,填 sigframe)各自碰一次
这个转换,**方向相反**。两边都必须逐字段拷。

`head` 也不能忘:`head.magic = FPSIMD_MAGIC`(0x46508001)、
`head.size = sizeof(struct fpsimd_context)`。内核的 `parse_user_sigframe()` 靠
magic 识别这个块,写错了 `rt_sigreturn` 直接 `SIGSEGV`。

---

## 5. `rt_sigreturn` —— 为什么最后一跳只能这么做

### 问题:寄存器不能一个一个设

restore 的最后一步是「让新进程的所有寄存器等于 dump 时的值」。看起来直白,
但有一个死结:

**执行「设置寄存器」这个动作本身需要用到寄存器。**

设 `sp`?那正在跑的代码就没栈了。设 `pc`?那就跳走了,后面的代码不再执行。
无论什么顺序,总有最后一个寄存器没法在「不破坏正在执行的代码」的前提下设进去。

用 ptrace 从外部设呢?那要求有另一个进程在外面操作 —— 而 B1 的模型是进程
**把自己变成**目标,没有外部操作者。(而且 ptrace 设完之后,detach 那一刻的语义
还要再讨论一次。)

### 内核早就解决了这个问题

信号处理返回时,内核必须**原子地**恢复被中断时的全部寄存器 —— 这和我们的需求
一模一样。它的做法是 `rt_sigreturn`:

1. 用户态在栈上准备一个 `struct rt_sigframe`,里面装着「要恢复成什么样」
2. 把 `sp` 指向它
3. 执行 `rt_sigreturn` 系统调用
4. **内核从这个 frame 恢复所有寄存器,然后返回用户态**

第 4 步在内核里发生,由内核的返回路径一次性完成。**这就是我们要的原子性。**
CRIU 用的不是一个技巧,是**内核为这件事提供的唯一机制**。

### aarch64 上就三条指令

```c
#define ARCH_RT_SIGRETURN(new_sp, rt_sigframe)					\
	asm volatile(								\
			"mov sp, %0					\n"	\
			"mov x8, #"__stringify(__NR_rt_sigreturn)"	\n"	\
			"svc #0						\n"	\
			:							\
			: "r"(new_sp)						\
			: "x8", "memory")
```

(`criu/compel/arch/aarch64/src/lib/include/uapi/asm/sigframe.h:46`)

`mov sp` 设栈顶 → `x8` 放系统调用号(aarch64 的系统调用号在 x8)→ `svc #0` 进内核。
**之后这段代码就不存在了** —— 内核不会返回到 `svc` 的下一条指令,而是返回到
frame 里那个 `pc`。

注意这里为什么必须是内联汇编而不是 `syscall(__NR_rt_sigreturn)`:libc 的封装会
先建自己的栈帧、可能用到 `sp`,而我们刚刚把 `sp` 指到了一个精心构造的 frame 上。
**从 `mov sp` 到 `svc` 之间不能有任何编译器插入的代码。**

### frame 里装什么

```c
struct rt_sigframe {
	siginfo_t info;
	ucontext_t uc;
	uint64_t fp;
	uint64_t lr;
};
```

(同一个头文件)

`uc.uc_mcontext` 里是 `regs[31]` / `sp` / `pc` / `pstate` / `fault_address`,
后面跟着 `aux_context`(`fpsimd_context` + 结束标记)。**这就是第 1 节那张表的
全部内容,以内核规定的布局排列。**

末尾的 `fp` / `lr` 是 CRIU 加的,不是内核结构的一部分 —— 内核只要求 `sp` 指向的
位置开始是 `info` + `uc`。

### 顺带解决的一件事:信号掩码

`uc.uc_sigmask` 也会被 `rt_sigreturn` 恢复。**所以信号掩码不需要单独一步。**
这解释了 A6 的一个设计:掩码存在 `core-$tid.img` 里(和寄存器一起),
而不是在 `sigacts` 里 —— 因为它和寄存器在同一时刻、由同一个机制恢复。

> 这是 A/B 划分的一个有意思的边缘案例:信号掩码是 B 类状态(内核替进程记的),
> 但它的恢复搭了 A 类状态的车。**「谁持有」和「怎么恢复」是两个不同的问题。**

---

## 6. 这一跳怎么和其他步骤接上

`rt_sigreturn` 只是最后一条指令。它成立要靠前面全部铺好:

```
1. B 类状态全部就位(fd、信号处理表、pid、session)
2. 目标的地址空间就位(premap → mremap,原理 03)
3. 在那个地址空间的某处构造好 rt_sigframe
4. mov sp, frame; mov x8, #__NR_rt_sigreturn; svc #0
   └─ 之后这段代码消失,进程「变成」目标
```

**第 4 步之后 restorer 自己的代码和栈都不在了**,所以任何还没做完的事情都永远做不了了。
这就是 [01-process-anatomy](01-process-anatomy.md) 里那条唯一的强序约束的来源:

> A 类恢复(地址空间替换)必须在所有 B 类恢复之后。

frame 放在哪也有约束:它必须在**替换之后仍然有效的**内存里 —— 也就是目标地址空间
里的某个位置,通常是目标某个线程栈的顶部。放在 restorer 自己的栈上是一个典型错误:
第 3 步和第 4 步之间地址空间已经换了,那块内存可能已经不存在。这是 B1 的一个
具体实现要点。

### 多线程时每个线程一份

每个线程有自己的 `rt_sigframe`,各自执行自己的 `rt_sigreturn`。它们**互不同步** ——
没有「所有线程一起跳」的必要,因为每个线程的 A 类状态是独立的。

这是 A4 那句「`mm` 存一份,寄存器每个都要」的 restore 侧对应物。

---

## 7. 延伸阅读

按「先看机制、再看 CRIU 怎么用」的顺序:

**内核侧:**

- `arch/arm64/kernel/signal.c` —— 本篇的主要依据。重点三处:
  - `do_signal()`(第 843 行)—— 第 3 节的系统调用重入
  - `setup_rt_frame()` / `setup_return()`(第 700-760 行)—— frame 怎么建的,
    **反过来读就是 B1 要怎么填**
  - `__sys_rt_sigreturn()` / `restore_sigframe()` —— frame 怎么被消费的
- `arch/arm64/kernel/fpsimd.c` —— `sve_sync_to_fpsimd()`、
  `fpsimd_preserve_current_state()`。第 4 节那个 SVE 陷阱的出处
- `arch/arm64/include/uapi/asm/sigcontext.h` —— `fpsimd_context` 的权威定义。
  **和 `uapi/asm/ptrace.h` 里的 `user_fpsimd_state` 对照读**,亲眼看一遍字段序是反的
- `arch/arm64/include/asm/processor.h:257` —— `task_pt_regs`
- `arch/arm64/kernel/ptrace.c` —— regset 表。**顺便能看到 5.10 里到底注册了哪些 PAC
  regset**,也就是第 0 节那个门禁的直接证据

**CRIU 侧:**

- `criu/criu/arch/aarch64/crtools.c` —— `save_task_regs()` / `restore_gpregs()`。
  A3 和 B1 各抄一半
- `criu/criu/arch/aarch64/sigframe.c` —— 那个 `return 0;`
- `criu/compel/arch/aarch64/src/lib/include/uapi/asm/sigframe.h` ——
  `ARCH_RT_SIGRETURN` 和 `struct rt_sigframe`
- `criu/criu/arch/aarch64/include/asm/parasite.h` —— TLS 为什么非得走 parasite
- `criu/images/core.proto` + `criu/images/core-aarch64.proto` —— 镜像里的字段
- `criu/criu/pie/restorer.c` 的 `restore_thread()` —— 最后一跳的完整上下文。
  **这是整个 CRIU 里最值得读一遍的一个函数**

**对照阅读(可选):** `criu/criu/arch/x86/crtools.c` 和
`compel/arch/x86/.../sigframe.h`。看一眼 x86 的 `xsave` 处理和 `orig_ax` 逻辑,
就能理解第 0 节那张对比表不是修辞。
