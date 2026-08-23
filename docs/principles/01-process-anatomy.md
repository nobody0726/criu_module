# 原理 01 —— 一个进程是由什么构成的

> 被引用于:[S0](../steps/S0-feasibility-spike.md)、[A1](../steps/A1-readonly-probe.md)

---

## 1. 核心问题:要复制一个进程,需要复制什么

「保存一个进程然后原样恢复」这句话听起来简单,但它立刻引出一个问题:
**一个进程到底是什么?**

不是「一个程序」——程序是磁盘上的文件,它不含运行状态。
不是「一段内存」——内存里没有「这个进程打开了哪些文件」。
不是「`task_struct` 这个结构体」——它里面全是指针,指向别处。

有用的答案是把进程状态分成两类。**这个二分是理解整个 C/R 的钥匙**,后面每一篇
原理文档都建立在它上面。

---

## 2. A 类与 B 类

| | **A 类** | **B 类** |
|---|---|---|
| 定义 | 进程**自己**持有的状态 | 内核**代它**持有的状态 |
| 在哪 | 进程的地址空间里、CPU 寄存器里 | 内核的各种结构体里 |
| 例子 | 堆、栈、全局变量、代码、`rip`、`rsp` | fd、信号处理表、定时器、pid、父子关系、socket |
| 怎么保存 | **直接拷贝字节** | 逐项读出各字段 |
| 怎么恢复 | **把字节写回去** | **重放系统调用** |

### A 类:字节就是全部

进程的堆里有一个 `int x = 42`。保存它就是把那 4 个字节存下来,恢复它就是把那 4
个字节写回同一个虚拟地址。没有任何语义需要理解 —— 内存就是一个巨大的字节数组,
拷贝它就完整保存了它。

寄存器同理:16 个通用寄存器 + `rip` + `rsp` + 标志位 + FPU 状态,一共几百字节。
存下来,恢复时写回去。

**A 类的恢复是「写入」。**

### B 类:字节毫无意义

进程打开了一个文件,得到 fd `3`。这个 `3` 是什么?它是
`task->files->fdt->fd[3]` 这个数组下标。

如果你把 `struct file` 的字节拷贝下来,restore 时再写回一块新分配的内存 —— 
**完全没用。** 因为:

- 那个 `struct file` 里的 `f_inode` 指向的 inode 在新系统里地址不同
- 它在内核的各种全局链表上(`sb->s_inodes`、LRU 链表)有链接,新拷贝不在这些链上
- 它的引用计数、锁状态,都是与内核当时的运行时状态绑定的

**唯一能重建一个 fd 的办法是调 `open()`。** 让内核自己去分配 `struct file`、
把它挂上所有该挂的链表、设好所有该设的计数。

**B 类的恢复是「重放系统调用」。**

### 一个判定法则

问自己:**这个状态如果我把它的字节整块搬到另一台机器上,还有意义吗?**

- 有意义 → A 类(内存里的 `42` 到哪都是 `42`)
- 没意义 → B 类(一个 `struct file *` 的数值到别处就是个野指针)

---

## 3. 「载体全新,内容全等」

初学时容易得出一个错误结论:「既然 B 类要重建,那恢复后的东西就和原来不一样了」。

**不对。** 正确的表述是:

> **载体全新,内容全等。**

- **载体全新**:每一个内核对象都是重新分配的。新的 `task_struct`、新的
  `struct file`、新的 `struct pid`、新的 `mm_struct`。原来那些内存已经被释放了,
  地址全都不同。
- **内容全等**:每一个**可观测**的值都和原来完全相同。fd 号相同、文件位置相同、
  信号掩码相同、**pid 相同**。

pid 尤其值得强调,因为它最反直觉。恢复后的进程 pid 与原来**完全相同**,而且这
不是「尽力而为」——CRIU 的代码里是硬断言的:

```c
	pid = getpid();
	if (vpid(current) != pid) {
		pr_err("Pid %d do not match expected %d\n", pid, vpid(current));
		set_task_cr_err(EEXIST);
		goto err;
	}
```

(`criu/criu/cr-restore.c:1552` 附近)

不等就直接失败退出。**不存在「pid 变了但其他都对」的恢复结果。**

### 为什么 pid 必须相等

因为 pid 会**泄漏进 A 类内存**。进程自己的内存里到处都可能存着自己的 pid:

| 泄漏途径 | 后果 |
|---|---|
| glibc 缓存的 `getpid()` 结果 | 进程自己算出的 pid 是旧的 |
| `pthread_t` / 线程 TID | `pthread_join` 找不到线程 |
| robust futex / `pthread_mutex` 的 owner TID | `unlock` 返回 `EPERM` |
| 写在文件里的 pidfile | 外部工具找错进程 |
| `/proc/self/...` 拼成的字符串 | 打开错误的路径 |
| SysV IPC 的 `msg_lspid`(最后操作者 pid) | 语义错误 |
| 文件锁的持有者 pid | 锁归属混乱 |

这些都是 A 类状态,**A 类是按字节恢复的,里面的 pid 不会被翻译。** 所以只有一条
路可走:**让 pid 去适配进程,而不是让进程去适配 pid。**

这就是 `clone3(set_tid)` 存在的理由 —— 它是 Linux 专门为 C/R 加的能力,让创建
进程时可以指定 pid。详见 [06-pid-and-session](06-pid-and-session.md)。

---

## 4. `task_struct` 的地图

内核里一个进程就是一个 `struct task_struct`(`include/linux/sched.h`)。它有
几百个字段,但对 C/R 来说只有这些要紧:

```
struct task_struct
  ├── pid, tgid                    ← 身份(注意命名与用户态相反,见下)
  ├── real_parent, parent          ← 父子关系
  ├── children, sibling            ← 子进程链表
  ├── group_leader, thread_group   ← 线程组
  ├── *mm  ─────────────────────►  struct mm_struct     ← A 类的容器
  │                                  ├── mmap (VMA 链表)
  │                                  ├── pgd (页表根)
  │                                  └── start_brk, brk, start_stack ...
  ├── *files ───────────────────►  struct files_struct  ← fd 表
  │                                  └── fdt->fd[]  → struct file *
  ├── *fs ──────────────────────►  struct fs_struct     ← cwd / root
  ├── *sighand ─────────────────►  struct sighand_struct
  │                                  └── action[_NSIG]  ← 信号处理表
  ├── *signal ──────────────────►  struct signal_struct ← 进程级共享
  │                                  ├── shared_pending
  │                                  ├── posix_timers
  │                                  └── it[] (itimers)
  ├── pending                      ← 本线程的待处理信号
  ├── blocked                      ← 本线程的信号掩码
  ├── *nsproxy ─────────────────►  各个 namespace
  ├── *cred ────────────────────►  uid/gid/capabilities
  ├── thread (struct thread_struct) ← fsbase/gsbase 等架构相关
  └── start_time, start_boottime   ← 启动时刻(只写一次,见 X1)
```

**除了 `mm` 指向的内容和寄存器,这张图上的一切都是 B 类。**

### 内核与用户态的命名倒挂

这是一个经典混淆源,值得单独记住:

| 内核字段 | 用户态对应的调用 |
|---|---|
| `task->pid` | **`gettid()`** —— 线程 id |
| `task->tgid` | **`getpid()`** —— 进程 id |

内核里没有「进程」这个一等概念,只有 task。**用户态说的「进程」= 内核里的
thread group**,用户态说的「线程」= 内核里的一个 task。

搞混的症状很有特征:**单线程时一切正常,多线程时所有线程 id 都变成了主线程的。**

### 共享是通过指针相等表达的

注意 `mm`、`files`、`fs`、`sighand` 都是**指针**。两个 task 的这些指针相等,
就意味着它们共享对应的资源:

| 指针相等 | 对应的 `clone()` flag | 用户态看到的 |
|---|---|---|
| `mm` 相同 | `CLONE_VM` | 共享地址空间 |
| `files` 相同 | `CLONE_FILES` | 共享 fd 表 |
| `fs` 相同 | `CLONE_FS` | 共享 cwd |
| `sighand` 相同 | `CLONE_SIGHAND` | 共享信号处理表 |
| 以上全部 + `CLONE_THREAD` | —— | 就是「线程」 |

**「线程」不是一个独立机制,它是这些共享的组合。** 这个认识让 A4(多线程)的
工作量比预想小很多:线程要 dump 的增量只是那些**没有**共享的东西。

而对 dump 侧,「判断两个 task 是否共享某资源」就是**比较指针**。CRIU 在用户态
做不到这件事,它必须靠 `kcmp()` 系统调用一对一对地问内核。这是内核模块的一个
真实优势。

---

## 5. 这个二分如何决定整个 C/R 的形状

A/B 二分带来一条**唯一的强顺序约束**,它是整个 restore 流程的骨架:

> **A 类恢复必须在所有 B 类恢复之后。**

理由:A 类恢复就是替换整个地址空间。而做替换的那段代码(restore 程序自己)
就住在地址空间里。**换完之后,它自己就不存在了。**

所以 restore 的形状必然是:

```
先做所有 B 类           ← 此时 restore 程序还活着,还能报错、还能重试
   ↓
最后一次性换掉地址空间   ← 单向阀门
   ↓
rt_sigreturn 跳进目标    ← 不返回
```

这条约束解释了 CRIU 那七个 `CR_STATE_*` 阶段为什么是那个顺序,也解释了为什么
restore 比 dump 复杂得多:**dump 是纯读,顺序随意、可重试、可并行;restore 是
一串不可逆的一次性动作。**

详见 [09-restore-ordering](09-restore-ordering.md)。

---

## 6. 延伸阅读

- `include/linux/sched.h` —— `task_struct` 的定义,直接读比读任何二手材料都好
- `kernel/fork.c` 的 `copy_process()` —— 一个 task 是怎么被造出来的。**这个函数
  就是「构造式恢复」的全部依据**:所有关系都在这里被填好
- `criu/criu/include/pstree.h` —— CRIU 用什么结构表示一棵待恢复的树
- [03-memory-and-vma](03-memory-and-vma.md) —— A 类的细节
- [07-fd-and-shared-objects](07-fd-and-shared-objects.md) —— B 类里最复杂的一种
