# 原理 08 —— 内核模块能做什么,不能做什么

> 被引用于:[S0](../steps/S0-feasibility-spike.md)、[X1](../steps/X1-start-time.md)、
> [B1](../steps/B1-mini-restore.md)

---

## 1. out-of-tree 模块的真实处境

一个 out-of-tree 内核模块**不是**「运行在内核里的程序」那么自由。它受三层限制:

| 层 | 限制 | 后果 |
|---|---|---|
| **链接期** | 只能调用 `EXPORT_SYMBOL` 的函数 | 大量内核内部函数不可用 |
| **编译期** | 结构体布局随 `CONFIG_*` 变化 | 换配置就要重编,读错字段不报错 |
| **运行期** | 上下文约束(能否睡眠、要持哪把锁) | 违反了不一定立刻崩,可能是随机的死锁或数据损坏 |

第三层最危险,因为**违反它往往不产生立即可见的错误**。这就是本项目把
`CONFIG_DEBUG_VM` + `CONFIG_PROVE_LOCKING` + `CONFIG_DEBUG_ATOMIC_SLEEP` 三个
选项作为不可关闭的开发环境要求的原因(见 [04-Dev-Environment](../04-Dev-Environment.md))
—— 它们把第三层的违规从「随机现象」变成「立即报错并打印调用栈」。

### `EXPORT_SYMBOL` vs `EXPORT_SYMBOL_GPL`

```c
EXPORT_SYMBOL(vm_insert_page);       /* 任何模块可用 */
EXPORT_SYMBOL_GPL(mmput);            /* 只有声明 GPL 许可的模块可用 */
```

`_GPL` 版本要求 `MODULE_LICENSE("GPL")`。本项目**必须**声明 GPL —— 不是法律偏好
问题,是技术必要:C/R 需要的绝大多数符号都是 `_GPL` 导出的。

### grep 到 `EXPORT_SYMBOL` 不等于可用

这是 S0 存在的全部理由。四类「grep 说可以,实际不行」:

1. **`_GPL` 差异** —— 见上
2. **头文件里没有声明** —— 符号导出了,但声明在 `mm/internal.h` 之类的地方,
   模块拿不到。硬办法是自己抄一份声明,但那意味着**你在赌 ABI**
3. **上下文不允许** —— 比如某函数要求已持 `mmap_lock`,或者不能在原子上下文调
4. **结构体字段偏移** —— 编译过了,读到垃圾。这是最坏的一类

**只有真编译真加载真读值,才能排除这四类。**

---

## 2. 一条自我约束:不用 `kallsyms_lookup_name()`

技术上,`kallsyms_lookup_name()` 能绕过 `EXPORT_SYMBOL`,按名字查任意符号地址。
本项目**明确禁止这种做法**,并由 CI 检查:

```bash
# Global Constraint: an unexported symbol is a finding to record, not a lock
# to pick.
if grep -rn "kallsyms_lookup_name\|kprobe_lookup_name" kernel_module/; then
	echo "FAIL: symbol lookup used to bypass EXPORT_SYMBOL"
	exit 1
fi
```

三个理由:

1. **它把编译期错误变成运行期错误。** 链接失败是当场、明确、无歧义的;
   `kallsyms_lookup_name()` 返回 NULL 或者返回一个语义已变的同名函数,是运行时的
   谜题。
2. **它绕过的是一个有意的边界。** 一个函数没被导出,通常意味着它的调用约定
   (锁、上下文、前置条件)只在内核内部成立。绕过导出检查不会让那些约定消失。
3. **5.7 之后 `kallsyms_lookup_name` 本身也不再导出了。** 要用它得先用 kprobe
   去找它 —— 一个绕过检查的手段,需要另一个绕过检查的手段。这个信号已经足够清楚。

**「一个符号拿不到,是一条要记录的结论,不是一把要撬的锁。」**

这条约束的实际价值是它**塑造了整个项目的形状**:正因为不撬锁,S0 才必须在第一周
把「哪些做不到」查清楚,而查清楚的结果直接决定了 restore 必须留在用户态。
如果允许撬锁,这个项目会用三个月走进一条死路。

---

## 3. 版本锁定:为什么是 5.10

**模块必须为一个确定的内核版本编译。** 本项目锁定 **5.10 LTS**(开发用 5.10.29)。

理由不是「5.10 比较好」,而是几个具体的结构变化:

| 变化 | 版本 | 影响 |
|---|---|---|
| `mm->mmap` 链表 → maple tree | **6.1** | `vma->vm_next` 字段被删除,**所有 VMA 遍历代码失效** |
| `mmap_sem` → `mmap_lock` | 5.8 | 函数名全变(`down_read(&mm->mmap_sem)` → `mmap_read_lock(mm)`) |
| pipe `nrbufs`/`curbuf` → `head`/`tail` | 5.5 | 读管道数据的代码要重写 |
| `clone3(set_tid)` 加入 | 5.5 | **低于此版本 restore 无法钉 pid** |
| `kallsyms_lookup_name` 不再导出 | 5.7 | (与本项目无关,因为不用它) |

**5.10 是「有 `clone3(set_tid)`,还有 `vm_next`」的那个区间里最新的 LTS。**
这是一个真实的工程选择:5.5–6.0 都满足,选 LTS 里最新的那个。

### 6.1 构建是一个故意的哨兵

CI 里有一个针对 6.1 的构建任务,**它预期失败**。它的作用是:

- 把「6.1 不兼容」从一句文档变成一个**可执行的事实**
- 如果有人某天让它过了,那就是一个真实的进展,而不是一个未经验证的猜测
- 它记录了失败的**具体位置**(哪一行用了 `vm_next`),这就是将来移植的工作清单

**一个 expected-failure 的 CI 任务,比一段「暂不支持 6.1」的文字有用得多。**

---

## 4. 内核模块的四个真实优势

这些不是「内核里做事更快」这种笼统说法,每一条都对应 CRIU 的一处具体困难:

| # | 优势 | CRIU 必须怎么做 | 我们怎么做 |
|---|---|---|---|
| 1 | **读信号处理表** | 注入 parasite 代码到目标进程里执行 `sigaction()` | 直接读 `task->sighand->action[]` |
| 2 | **读所有线程的寄存器** | 对每个线程 `PTRACE_ATTACH` + `GETREGSET` | `task_pt_regs(t)`,宏,零系统调用 |
| 3 | **判断资源共享** | `kcmp()` 逐对询问 + 两级 rbtree 近似 | **比较指针**,定义上就正确 |
| 4 | **读管道未读数据** | `tee()`/`splice()`,有容量限制 | 直接读 `pipe_inode_info->bufs[]` |

**第 1 条是最大的一条。** parasite 是 CRIU 最复杂的机制:编译一段位置无关代码
(整个 `compel` 工具链就为这个存在)、写进目标的地址空间、劫持寄存器让它执行、
收结果、恢复现场。内核模块**完全不需要它**,因为那些状态本来就在眼前。

**第 3 条最容易被低估。** CRIU 的 `kcmp` 两级树是一个精心设计的近似:先用 fstat
的 `(dev, ino, mnt_id)` 分组,同组内才调 `kcmp()`。它正确,但它是一个需要维护的
机制,有自己的边界情况。指针相等没有边界情况。

---

## 5. 三个 restore 必须留在用户态的理由

对称地,内核模块在 restore 侧有硬性障碍。每一条都追溯到一个具体机制:

### 5.1 造不出用户进程

造一个用户进程需要 `mm_alloc()`、`vm_area_alloc()`、`insert_vm_struct()`、
`alloc_pid()`。**这些全部未导出**(S0 组 4 的验证目标)。

`kthread_create()` 不是替代品:内核线程**没有 `mm`**(`task->mm == NULL`),
而且它的父进程是 `kthreadd` 而不是你想要的那个进程。你没法把一个内核线程
「变成」一个用户进程 —— 那需要的正是上面那些未导出的函数。

### 5.2 钉不住 pid

`alloc_pid()` 未导出,所以内核模块无法指定新任务的 pid。而**用户态有
`clone3(set_tid)`** —— 一个专门为此存在的、稳定的、有文档的接口。

**这是整个架构决策里最干脆的一条:内核里做不到的事,用户态有一个专用系统调用。**

### 5.3 地址空间替换需要「站在被替换的空间里」

premap → bootstrap → mremap 那三拍(见 [03](03-memory-and-vma.md))本质上要求
**执行替换的代码就住在被替换的地址空间里**,并且在最后一刻通过 `rt_sigreturn`
把自己「变成」目标进程。

内核模块不住在任何用户地址空间里,它没法「变成」目标进程。它能做的只是操作
另一个进程 —— 而那需要的又是 5.1 里那些未导出的函数。

### 结论

```
dump    → 内核模块(四个优势都在这一侧)
restore → 用户态(三个障碍都在这一侧)
```

**这不是妥协,这是被机制决定的。** 而且它有一个意外的好处:两侧的接口是
**磁盘上的文件**,所以可以各自用真 CRIU 的另一半来验证。整个迭代计划的可测试性
建立在这个分界上。

---

## 6. X1:唯一一个「只有内核能做」的能力

上面说 restore 必须在用户态,但有**恰好一个**例外:`task->start_time`。

- 它在 `copy_process()` 里被写一次,之后**没有任何写入路径**
- 没有系统调用能改它
- `/proc/PID/stat` 的第 22 个字段读它,`ps` 的 `etimes`/`lstart` 依赖它

所以 CRIU **恢复不了进程的启动时间**。`criu/images/core.proto` 里字段 19 就是
为它预留的,**被注释掉了** —— 因为用户态没有写入路径,存了也没用。

这使 [X1](../steps/X1-start-time.md) 成为整个项目里唯一一个「内核模块提供了
CRIU 无法提供的能力」的步骤。**它的价值不在这一个字段有多重要,在于它证明了
这条路存在。**

### 它的安全代价(来自 X1 §4.3)

一个能修改任意进程 `start_time` 的接口,滥用面必须写清楚:

| 顾虑 | 评估 |
|---|---|
| 能否用来提权 | 否。`start_time` 不参与任何权限判断 |
| 能否用来隐藏进程 | 部分能 —— 能干扰 `pgrep -o`、`ps` 排序,以及基于运行时长的审计 |
| 能否造成内核崩溃 | 否,只是两个 `u64` 字段。但下溢会让时间计算给出荒谬结果 |
| 谁能调用 | 要求 `CAP_SYS_ADMIN`,和 `criu dump` 本身同一级别 |

**结论:滥用面局限在「干扰审计工具的时间视图」。** 一个已经拿到 `CAP_SYS_ADMIN`
的攻击者有远比这更直接的手段,所以增量风险很小 —— 但不是零。

**而这正是它不适合进上游内核的理由之一。** 上游要为所有人承担这个接口的存在,
而收益只对 C/R 场景成立。作为一个需要显式加载的 out-of-tree 模块,它的存在位置
是合理的:**用它的人知道自己在做什么。**

不要假装它没有代价。写出代价是这个接口能被接受的前提。

---

## 7. 一份「不要用」清单

S0 会实验复核,但已有的 grep 结论值得先记在这里 —— 它们是原设计大纲里出现过的、
**看起来能用但不该用**的符号:

| 符号 | 状态 | 为什么不用 |
|---|---|---|
| `mm_alloc` | 未导出 | 造进程用,拿不到 |
| `vm_area_alloc` | 未导出 | 同上 |
| `insert_vm_struct` | 未导出 | 同上 |
| `alloc_pid` | 未导出 | **pid 恢复在内核里做不了的直接证据** |
| `kernel_execve` | 未导出 | —— |
| `find_get_task_by_vpid` | 未导出 | 用 `find_vpid` + `get_pid_task` 代替 |
| `mmget_not_zero` | 未导出 | 用 `get_task_mm` 代替 |
| `follow_page` | 未导出 | 用 `get_user_pages_remote` 代替 |
| `do_mmap` / `do_munmap` | 未导出(MMU 路径) | `vm_mmap` / `vm_munmap` 已导出 |
| **`vm_insert_page`** | **已导出** | **能链接但会毁掉语义 —— 见下** |

### `vm_insert_page` 单独说

它是这张表里唯一一个「能用但不能用」的:

```c
	if (!(vma->vm_flags & VM_MIXEDMAP)) {
		BUG_ON(mmap_read_trylock(vma->vm_mm));
		BUG_ON(vma->vm_flags & VM_PFNMAP);
		vma->vm_flags |= VM_MIXEDMAP;
	}
```

(`linux-5.10.29/mm/memory.c:1821` 附近)

**它强行给 VMA 打上 `VM_MIXEDMAP`。** 那个 flag 的本意是「这个 VMA 里混着有
`struct page` 和没有 `struct page` 的页」,是给设备驱动用的。一个普通匿名 VMA
被打上它之后,COW 行为、`get_user_pages` 快路径、`fork()` 的页表复制分支、
`smaps` 统计全部改变。

**症状不是崩溃,是语义悄悄坏掉。** 这类符号比未导出的符号危险得多 —— 未导出的
在链接期就拦住你了。

**判据:一个导出符号是否可用,要看它的副作用是否在你的语义预算内,
而不只是看它能否链接。**

---

## 8. 实验复核(S0 填写)

> 这一节由 [S0](../steps/S0-feasibility-spike.md) 的实验结果填入。在 S0 完成前,
> 上面所有「未导出」的结论都只有 grep 依据,**没有实验依据**。

| # | 待验证 | grep 结论 | 实验结论 | 环境 | 日期 |
|---|---|---|---|---|---|
| 1.2 | `pid_task(find_vpid(nr), PIDTYPE_PID)` | 待验 | | | |
| 1.3 | `get_pid_task` / `put_task_struct` | 待验 | | | |
| 1.4 | 不加 `rcu_read_lock()` 时 lockdep 是否报警 | —— | | | |
| 2.2 | `get_task_mm` / `mmput` | `mmput` 已导出 | | | |
| 2.3 | `mmap_read_lock(mm)` | inline,应可用 | | | |
| 2.4 | `vma->vm_next` 遍历 vs `/proc/PID/maps` 行数 | 5.10 有 `vm_next` | | | |
| 2.5 | `d_path()` 取 `vm_file` 路径 | 待验 | | | |
| 3.2 | `get_user_pages_remote` | 已导出 | | | |
| 3.3 | `access_process_vm` | 已导出 | | | |
| 3.4 | 读未映射地址的行为 | —— | | | |
| 3.5 | 读 `PROT_NONE` guard page 的行为 | —— | | | |
| 4.1 | `mm_alloc` | 未导出 | | | |
| 4.2 | `vm_area_alloc` | 未导出 | | | |
| 4.3 | `insert_vm_struct` | 未导出 | | | |
| 4.4 | `do_mmap` / `vm_mmap` | 前者未导出 | | | |
| 4.5 | `do_munmap` / `vm_munmap` | 前者未导出 | | | |
| 4.6 | `alloc_pid` | 未导出 | | | |
| 4.7 | `kernel_execve` | 未导出 | | | |
| 4.8 | `vm_insert_page` | 已导出但有副作用 | | | |

**「环境」一栏要写内核版本和 `CONFIG_*` 关键选项**,因为第 1 节的第 4 类问题
(字段偏移随配置变化)意味着一个结论只在一个配置下成立。

---

## 9. 延伸阅读

- `include/linux/export.h` —— `EXPORT_SYMBOL` 宏的实现,看它怎么生成符号表
- `Documentation/process/stable-api-nonsense.rst` —— **内核为什么不承诺内部 ABI
  稳定。读它能理解本篇所有限制的根源**
- `kernel/module.c` 的 `resolve_symbol()` —— 模块加载时符号是怎么被解析的,
  包括 GPL 检查
- `mm/memory.c:1821` —— `vm_insert_page()`
- `criu/criu/kerndat.c` —— CRIU 的运行时特性探测,**S0 的思路来源**
- `criu/criu/cr-check.c` —— `criu check --all` 的实现
- `Documentation/dev-tools/kmemleak.rst` / `lockdep-design.rst` —— 那三个 DEBUG
  选项在查什么
- [01-process-anatomy](01-process-anatomy.md) —— A/B 二分,它决定了哪些能力必须
  在哪一侧
- [../04-Dev-Environment.md](../04-Dev-Environment.md) —— 三个 DEBUG 选项的具体配置
