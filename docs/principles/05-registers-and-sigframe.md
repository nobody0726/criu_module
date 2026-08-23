# 原理 05 —— 寄存器,以及 rt_sigreturn 这个魔法

> 被引用于:[A3](../steps/A3-minimal-dump.md)、[A4](../steps/A4-threads.md)、
> [A6](../steps/A6-signals-timers.md)、[B1](../steps/B1-mini-restore.md)

---

## 1. 一个线程的「执行位置」是什么

一个正在运行的线程,它的「当前位置」完全由 CPU 寄存器决定:

| 寄存器 | 含义 |
|---|---|
| `rip` | 下一条要执行的指令地址 |
| `rsp` | 栈顶 |
| `rbp` | 帧指针 |
| `rax`..`r15` | 通用寄存器,函数参数、局部变量、中间结果 |
| `eflags` | 标志位(比较结果、进位等) |
| `cs`/`ss`/`ds`/`es`/`fs`/`gs` | 段寄存器 |
| `fs_base`/`gs_base` | 段基址 —— **TLS 就靠它** |
| FPU/SSE/AVX 状态 | 浮点和向量寄存器,几百到几千字节 |

**这些加起来就是「执行状态」的全部。** 把它们存下来,恢复时写回去,线程就从
原来那条指令继续执行。

这是纯 A 类状态 —— 就是一堆字节,没有任何需要向内核重建的语义。

---

## 2. 从内核里读寄存器

### `task_pt_regs`

一个被停下来的任务,它的用户态寄存器保存在**内核栈的顶端**。内核在从用户态
进入内核态时(系统调用、中断、异常)会把它们压在那里。

```c
#define task_pt_regs(task) \
({									\
	unsigned long __ptr = (unsigned long)task_stack_page(task);	\
	__ptr += THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;		\
	((struct pt_regs *)__ptr) - 1;					\
})
```

(`arch/x86/include/asm/processor.h:763` 附近)

就是「内核栈顶往下退一个 `pt_regs` 的大小」。

**它是一个宏,所以不需要任何导出符号。** 对内核模块来说这很重要 —— 
`EXPORT_SYMBOL` 的限制在这里完全不适用。

对比 CRIU:它必须对**每个线程**单独 `PTRACE_ATTACH` + `PTRACE_GETREGSET`。
内核模块 `task_pt_regs(t)` 对任意线程直接可用。**这是内核模块的第二个真实优势**
(第一个是不需要 parasite)。

### 前提:任务必须真的停下来了

`task_pt_regs()` 只在任务被调度出去、上下文完整保存之后才可靠。如果任务还在
CPU 上跑,那块内存里是上一次进入内核时的旧值。

这就是 [02-freezing](02-freezing.md) 里强调「FROZEN ≠ 寄存器可读」的原因:
写完 `FROZEN` 必须轮询 `task_is_running()` 直到所有线程都真的停了。

### `pt_regs` 与 `user_regs_struct` 不是一回事

| | `struct pt_regs` | `struct user_regs_struct` |
|---|---|---|
| 定义在 | `arch/x86/include/asm/ptrace.h` | `arch/x86/include/uapi/asm/ptrace.h` |
| 用途 | 内核内部 | ptrace ABI,给用户态看的 |
| 字段顺序 | 内核压栈顺序 | ABI 规定的顺序 |

CRIU 的 `core-x86.proto` 用的是 **ptrace ABI 的语义**。x86_64 上两者布局很接近,
但**不能假设逐字段对应**。要按字段名逐个赋值,不要 `memcpy` 整个结构体。

### 段寄存器和 TLS

`fs_base` 是 TLS 的实现基础:`__thread` 变量的访问被编译成 `%fs:offset` 形式的
寻址。所以每个线程的 `fs_base` 不同,而且**必须精确恢复**,否则所有 TLS 变量都
会指向错误的地方。

`core-x86.proto` 的字段 22/23 就是它们:

```protobuf
	required uint64			fs_base		= 22;
	required uint64			gs_base		= 23;
```

内核侧在 `task->thread.fsbase`。但如果线程正在 CPU 上跑,真值在 MSR 里,
所以更稳妥的读法是 `x86_fsbase_read_task(t)`。任务已冻结时直接读字段通常也对,
**但这个假设必须写在注释里。**

---

## 3. `orig_ax`:一个容易误解的字段

`core-x86.proto:29`:

```protobuf
	required uint64			orig_ax		= 16;
```

x86_64 上系统调用号放在 `rax`,返回值也放在 `rax` —— 它们会互相覆盖。所以内核
在进入系统调用时把原始的 `rax`(即系统调用号)另存一份到 `orig_ax`。

`orig_ax` 的取值:

| 值 | 含义 |
|---|---|
| ≥ 0 | 任务**正在系统调用中**,这是系统调用号 |
| -1 | 任务不在系统调用中(被中断或异常打断) |

### 为什么这个字段对 C/R 很重要

考虑一个在 `pause()` 里被冻结的进程。它的状态是:

- `rip` 指向 `pause()` 之后的下一条指令(系统调用已经进去了)
- `orig_ax` = `__NR_pause`
- `rax` = `-ERESTARTNOHAND`(内核标记「这个系统调用需要重启」)

恢复时,内核看到 `rax` 是 `-ERESTARTNOHAND` 且 `orig_ax` 是有效的系统调用号,
就会**把 `rip` 回退一条指令、把 `rax` 恢复成 `orig_ax`、重新执行这个系统调用**。

**结果:进程恢复后仍然阻塞在 `pause()` 里,和 checkpoint 时一样。**

这就是系统调用重启机制,它是「被阻塞在系统调用里的进程也能被正确恢复」的原因。

**所以 A3 里那句「`orig_ax` 会是 `__NR_pause`,这是对的,不要试图把它归零」
不是权宜之计,是必须如此。** 归零会导致进程恢复后从 `pause()` 返回,以为自己
收到了信号。

---

## 4. FPU 状态:格式很挑剔

FPU/SSE/AVX 状态用 `xsave` 指令保存,格式是:

```
[512 bytes]  传统 FXSAVE 区域(x87 + SSE 的 xmm0-15)
[64 bytes]   xsave header,里面有 xstate_bv 位图
[变长]       扩展状态区域(AVX 的 ymm 高半、AVX-512 的 zmm ...)
```

`xstate_bv` 位图声明「哪些状态实际存在这块内存里」。**位图和实际内容必须一致**,
否则 `xrstor` 会加载垃圾,或者返回错误。

区域的总大小取决于 CPU 支持哪些特性,通过 `CPUID` 查询。

**结论:照抄 CRIU 的处理,不要自己算。** 复用
`criu/criu/arch/x86/sigframe.c` 和 `criu/compel/arch/x86/` 里的定义。

填错的症状有两种,后一种更糟:
- `rt_sigreturn` 返回 `-EFAULT`(响亮,好调试)
- **成功了但浮点寄存器是垃圾**(静默,恢复后的进程算出错误的浮点结果)

---

## 5. `rt_sigreturn`:恢复寄存器的唯一办法

现在到了最关键的部分。

### 问题

你有一整套目标寄存器的值,你要让当前线程「变成」那个状态。逐个设是不行的:

```
设 rax  ✓
设 rbx  ✓
...
设 rsp  ✓   ← 栈换了,局部变量全部失效
设 rip  ✗   ← 一旦设了 rip 就跳走了,后面的寄存器永远没机会设
```

**必须原子地一次全部设好。** 而 `rip` 必然是最后一个,可它一设就跳走。

用户态没有「原子地设置所有寄存器」的指令。`iret` 只能在内核态用。

### 内核已经有这个机制了 —— 信号

内核在投递信号时做了什么:

```
1. 保存当前所有寄存器(通用 + 段 + eflags + FPU + 信号掩码)
   打包成一个 struct rt_sigframe,放到用户栈上
2. 设置 rsp 指向那个 sigframe,rip 指向 handler
3. handler 执行
4. handler 返回时跳到 sa_restorer,它调 rt_sigreturn
5. 内核从栈上那个 sigframe 里把所有寄存器读回来,原子恢复
6. 线程回到被信号打断的那条指令,像什么都没发生过
```

**第 5 步就是我们要的那个原子操作。** 内核每天都在做这件事。

### CRIU 反过来用它

**手工在栈上构造一个 `rt_sigframe`,填进目标进程 checkpoint 时的寄存器值,
然后直接调 `rt_sigreturn`。**

内核不知道这不是真的信号返回 —— 它照常把那些值加载进寄存器,然后「返回」到
`rip` 指向的位置,也就是目标进程被 checkpoint 的那条指令。

实现在 `criu/criu/pie/restorer.c:681`:

```c
static void noinline rst_sigreturn(unsigned long new_sp, struct rt_sigframe *sigframe)
{
	ARCH_RT_SIGRETURN_RST(new_sp, sigframe);
}
```

而那个宏(x86_64 native 分支,定义在
`criu/compel/arch/x86/src/lib/include/uapi/asm/sigframe.h:198`)展开就是**三条指令**:

```
	movq %0, %%rax          ; new_sp -> rax
	movq %%rax, %%rsp       ; 把栈指针指向构造好的 sigframe
	movl $__NR_rt_sigreturn, %%eax
	syscall
```

**整个 A 类状态的恢复,最终归结为这三条指令。** 这是 CRIU 里最优雅的一处设计。

### 顺带恢复的东西

`rt_sigframe` 里除了寄存器还有 `uc_sigmask` —— **信号掩码也是被
`rt_sigreturn` 一起恢复的。** 所以恢复信号掩码不需要额外调 `sigprocmask()`,
把值填进 sigframe 就行。

这也解释了为什么 `blocked` 掩码存在 `core-$tid.img` 里(和寄存器一起)而不是
`sigacts` 里:它是「随 sigframe 恢复」的那一类。

### `sa_restorer` 为什么不能重算

用户态的 `struct sigaction` 有个 `sa_restorer` 字段,指向 libc 里那段调
`rt_sigreturn` 的代码。它必须**原样保存原样恢复**:

- 它是 libc 内部实现细节,地址在 libc 的代码段里
- restore 时整个地址空间是按原样恢复的,所以那个地址仍然有效
- **试图重新计算它意味着你要知道当前 libc 把那段代码放在哪** —— 而目标进程的
  libc 可能是另一个版本

内核里它在 `task->sighand->action[i].sa.sa_restorer`。存字节,别动脑。

---

## 6. 整条链是怎么闭合的

把这一篇和 [03](03-memory-and-vma.md) 的 premap/mremap 接起来,restore 的最后
三步是:

```
1. premap 好目标的所有 VMA,内容填好           ← A 类内容就位,但地址不对
2. 跳到 bootstrap,unmap 旧空间,mremap 到最终地址  ← A 类地址就位
   此时:目标的内存全部正确,栈上有构造好的 sigframe
   但当前执行的还是 restore 的代码
3. rt_sigreturn                                ← A 类寄存器就位,不返回
```

第 3 步之后,这个 task 从各种可观测角度看,就**是**原来那个进程了:
内存一致、寄存器一致、pid 一致、fd 一致、信号状态一致。

**载体全新,内容全等。**

---

## 7. 延伸阅读

- `arch/x86/kernel/signal.c` 的 `setup_rt_frame()` 和 `sys_rt_sigreturn()` —— 
  内核两个方向的实现,**读它们是理解这一篇的最好办法**
- `arch/x86/include/asm/processor.h` —— `task_pt_regs` 的定义
- `criu/criu/arch/x86/sigframe.c` —— CRIU 怎么填 sigframe
- `criu/compel/arch/x86/src/lib/include/uapi/asm/sigframe.h` —— `rt_sigframe`
  的结构定义和那几个汇编宏
- `criu/images/core-x86.proto` —— 字段清单
- `man 2 rt_sigreturn` —— 简短但值得一读,它明确说了「这个系统调用不该被程序直接
  调用」,而 CRIU 正是那个例外
- [03-memory-and-vma](03-memory-and-vma.md) —— premap/mremap
- [09-restore-ordering](09-restore-ordering.md) —— 为什么这一步必须最后做
