# 原理 03 —— 地址空间、VMA、以及 MAP_SHARED 到底是什么

> 被引用于:[A1](../steps/A1-readonly-probe.md)、[A3](../steps/A3-minimal-dump.md)、
> [A8](../steps/A8-shared-resources.md)

---

## 1. 地址空间是一张稀疏地图

x86_64 上一个用户进程的虚拟地址空间是 128TB(47 位)。它绝大部分是空的。
真正被使用的部分是一段一段的区间,每一段有自己的权限和来源:

```
0x400000        [代码段]     r-xp   来自可执行文件
0x600000        [数据段]     rw-p   来自可执行文件
0x601000        [堆 heap]    rw-p   匿名
...
0x7f....        [libc.so]    r-xp   来自 /lib/x86_64-linux-gnu/libc.so.6
0x7f....        [libc 数据]  rw-p   来自同一个文件,但可写
...
0x7ffd....      [栈 stack]   rw-p   匿名,向下增长
0x7ffff7f...    [vvar]       r--p   内核映射
0x7ffff7f...    [vdso]       r-xp   内核映射
0xffffffffff600000 [vsyscall] --xp
```

内核用 **VMA**(Virtual Memory Area,`struct vm_area_struct`)表示每一段:

```c
struct vm_area_struct {
	unsigned long vm_start;		/* 起始地址 */
	unsigned long vm_end;		/* 结束地址(不含) */
	struct vm_area_struct *vm_next;	/* 链表(5.10);6.1+ 改成了 maple tree */
	pgprot_t vm_page_prot;
	unsigned long vm_flags;		/* VM_READ / VM_WRITE / VM_SHARED ... */
	struct file *vm_file;		/* 文件映射的话,指向文件 */
	unsigned long vm_pgoff;		/* 文件内偏移(以页为单位) */
	...
};
```

`/proc/PID/maps` 就是这个链表的文本渲染。CRIU 靠解析这个文本文件工作
(`parse_smaps()` 在 `criu/criu/proc_parse.c`),而内核模块直接遍历链表。

> **版本陷阱:** 5.10 用 `vma->vm_next` 单链表;6.1 换成了 maple tree,
> `vm_next` 字段被删除。这是本项目锁定 5.10 的直接原因之一。

---

## 2. VMA 的四种类型:两个正交的轴

`mmap()` 的 flags 里有两组互相独立的选择,很多人把它们混在一起理解,导致
`MAP_SHARED` 变成一个模糊概念。**实际上是两个正交的轴。**

### 轴一:内容从哪来

| | 说明 |
|---|---|
| **file-backed**(文件映射) | 给了 `fd`,内容来自那个文件 |
| **anonymous**(匿名) | 给了 `MAP_ANONYMOUS`,内容初始为全零 |

### 轴二:写入的可见范围

| | 说明 |
|---|---|
| **MAP_PRIVATE** | 写入只有自己看得见。写时复制(COW) |
| **MAP_SHARED** | 写入对所有映射同一对象的人可见 |

### 二乘二

|  | MAP_PRIVATE | MAP_SHARED |
|---|---|---|
| **匿名** | 堆、栈、`malloc` 的大块内存 | fork 后父子共享的内存 |
| **文件** | 可执行文件的代码段、libc | mmap 一个文件当共享缓冲区 |

这四格正好对应 CRIU 的四个 VMA 标志:

| 象限 | CRIU 标志 | 内核侧判据 |
|---|---|---|
| 匿名 + private | `VMA_ANON_PRIVATE` | `vma_is_anonymous(vma) && !(vm_flags & VM_SHARED)` |
| 匿名 + shared | `VMA_ANON_SHARED` | `vma_is_shmem(vma) && (vm_flags & VM_SHARED)` |
| 文件 + private | `VMA_FILE_PRIVATE` | `vma->vm_file && !(vm_flags & VM_SHARED)` |
| 文件 + shared | `VMA_FILE_SHARED` | `vma->vm_file && (vm_flags & VM_SHARED)` |

### 一个必须记住的陷阱

**`VMA_ANON_SHARED` 的 `vm_file` 不是 NULL。**

`mmap(NULL, n, ..., MAP_SHARED | MAP_ANONYMOUS, -1, 0)` 看起来是匿名的,但内核
为它偷偷创建了一个 **shmem(tmpfs)inode**,因为「多个进程共享同一块内存」需要
一个共同的后备对象来锚定那些物理页。

所以:

```c
	/* WRONG: an anonymous MAP_SHARED region has a vm_file (an internal shmem
	 * inode), so this misclassifies it as file-backed.
	 */
	if (!vma->vm_file)
		return CRIU_VMA_ANON_PRIVATE;

	/* RIGHT: use the kernel's own predicates. */
	if (vma_is_anonymous(vma))
		return CRIU_VMA_ANON_PRIVATE;
	if (vma_is_shmem(vma) && (vma->vm_flags & VM_SHARED))
		return CRIU_VMA_ANON_SHARED;
```

**`vma_is_anonymous()` 只对真正的私有匿名映射返回真。** 匿名共享映射要用
`vma_is_shmem()` 判断。

这个 shmem inode 在文件系统里**没有路径** —— 它在一个内部 tmpfs 里,不在任何
挂载点下。所以 restore 时没法 `open()` 它,必须用「锚点」重建(见
[A8](../steps/A8-shared-resources.md))。

---

## 3. MAP_SHARED 是 A 类还是 B 类

这是一个值得单独想清楚的问题。

一块共享内存里的字节,看起来是 A 类(就是内存嘛,拷贝就行)。但:

**共享关系本身是 B 类。**

考虑父子进程共享一块匿名 `MAP_SHARED` 内存。如果你按 A 类处理 —— 把父进程的
那段内存存一份、子进程的那段也存一份,restore 时各自写回 —— 结果是:

- 两个进程各有一块**内容正确**的内存
- 但它们**不再共享**。父进程写入,子进程看不到

**内容对了,关系错了。** 而「关系」恰恰是共享内存唯一的存在理由。

正确的处理是把共享内存当成一个**对象**:

```
1. 给这个对象分配一个 id(内核里用 inode 指针做 key 去重)
2. 内容只存一份
3. 每个映射它的进程,在自己的 VMA 记录里引用那个 id
4. restore 时:第一个进程创建这个对象并填内容,其余进程 mmap 同一个对象
```

**这就是 B 类的处理方式:重建对象,而不是拷贝字节。**

### 内容存几份决定正确性,不只是空间

「存两份」的错误很隐蔽,因为它的症状不是「浪费空间」:

restore 端按 id 去重。如果你给同一块共享内存分配了两个不同的 id,restore 端会
**建两块独立的内存**。恢复后的两个进程各自有正确的内容,但互相看不到对方的写入。

**而且单向使用共享内存的测试查不出这个错误。** 只有双向检验(A 写 B 读、
B 写 A 读)才能抓到。这与 [07](07-fd-and-shared-objects.md) 里 pipe 两端配对
错误是完全同一类问题。

---

## 4. 哪些页需要存,哪些不需要

一个 1GB RSS 的进程,不代表要存 1GB。CRIU 的 `should_dump_page()`
(`criu/criu/mem.c:105-175`)做了几层筛选。

### 按 VMA 类型筛

`criu/criu/mem.c:451`:

```c
	if (!vma_area_is_private(vma, kdat.task_size) &&
	    !vma_area_is(vma, VMA_ANON_SHARED))
```

| VMA 类型 | 内容进 pages 文件? | 谁是权威副本 |
|---|---|---|
| `VMA_ANON_PRIVATE` | **是** | pages 文件 |
| `VMA_FILE_PRIVATE` | 只存**已被写过**(已 COW)的页 | pages 文件 + 原文件 |
| `VMA_ANON_SHARED` | **是**(整个对象存一份) | pages 文件 |
| `VMA_FILE_SHARED` | **否** | 文件本身 |

`VMA_FILE_SHARED` 一个字节都不存,因为脏页会被内核回写到文件里 —— **文件就是
权威副本。**

这带来一个真实的语义缺口:如果 dump 之后有别人改了那个文件,restore 后的进程
看到的是改过的内容。**CRIU 也有这个缺口,它是设计不是 bug。**

### 按页是否存在筛

一块 `malloc` 了 1GB 但只写了 1MB 的内存,那 1023MB 从未被访问过,**根本没有对应
的物理页**(内核的按需分页)。读它会触发缺页处理,内核会**当场分配一个零页**。

所以如果你用 `access_process_vm()` 老实地读整个区间,你会:

1. 让目标进程凭空多出 1023MB 的 RSS
2. 在镜像里存 1023MB 的零

**这不只是性能问题,它改变了被 dump 的进程的状态**,违反了「dump 是纯读」。

正确做法是先判断页是否 present,再读。CRIU 读 `/proc/PID/pagemap` 二进制接口;
内核模块直接走页表:

```c
	/* Walk the page tables without faulting anything in. An absent entry at
	 * any level means the page was never allocated: reading it would create
	 * a zero page the process never had.
	 */
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return false;
	/* p4d, pud, pmd ... */
	return pte_present(*pte);
```

### 特殊区域

| 区域 | 处理 |
|---|---|
| **vDSO** | **总是整段 dump**,不做任何跳过优化 |
| **AIORING** | 同上 |
| `PROT_NONE`(guard page) | 不 dump —— restore 时也写不进去 |
| `VM_IO` / `VM_PFNMAP`(设备映射) | 不能 dump,应明确报错 |

vDSO 是内核映射进每个进程的一小段代码(让 `gettimeofday()` 不用真的进内核)。
它的地址和内容跨内核版本会变,所以 CRIU 整段存下来,restore 时做符号级重定位
(`criu/criu/pie/util-vdso.c`)。

**dump 侧只需要正确标记它是 vDSO**(`VMA_AREA_VDSO`)。忘记标记是 A3 最常见的
失败原因,而且症状(restore 在 vDSO 重定位阶段报错)不容易联想到原因。

---

## 5. pages 与 pagemap 的分工

CRIU 把内存内容拆成两个文件,理解这个分工能省掉两个 bug:

```
pages-1.img       纯裸字节。没有头部(除了 magic)、没有分隔符,一页接一页
pagemap-$pid.img  索引。「虚拟地址 X 起的 N 个页,在 pages 文件里从第 M 页开始」
```

为什么这么分:`pages` 文件可以用 `splice()` 零拷贝写入,**不能掺任何元数据**。

两个具体陷阱:

1. **`pagemap` 里的偏移单位是「页」,不是字节。**
2. **文件的第一条记录是 `pagemap_head`**(携带 `pages_id`),不是真正的映射记录。

参照 `criu/images/pagemap.proto`。

连续的可 dump 页要**合并成一条记录**(`nr_pages > 1`),否则一个 1GB 的进程会
产生 262144 条记录。

---

## 6. 恢复地址空间:premap → mremap

这是 A 类恢复的核心手法,值得单独理解。

**问题:** 目标进程的 VMA 要恢复到原来的虚拟地址,但那些地址现在被 restore 程序
自己占着(它的代码、栈、libc 都在某处)。不能先 unmap 自己 —— unmap 之后就没有
代码继续执行了。

**解法分三拍:**

```
第一拍 premap:  把目标的所有 VMA 先 mmap 到一块空闲的临时区域,内容填好。
                此时自己的地址空间还完整,还能报错、还能放弃。

第二拍 搬家:    把恢复参数、sigframe、以及第三拍的代码搬到一块两边都不冲突的
                区域(bootstrap),跳到那段代码上执行。

第三拍 mremap:  unmap 旧的一切(包括原来的 libc 和栈),然后把 premap 区域
                mremap 到最终地址。
```

**关键是 `mremap` 而不是 `memcpy`。** `mremap` 只改页表项,不搬数据,而且能把
一段映射移到任意目标地址。这让「内容已就位、地址还不对」变成一个纯页表操作,
而且是**原子的** —— 不存在「中间态没有内存」的窗口。

`bootstrap` 区域的作用是给第三拍提供一个「站得住的地方」:它不在目标进程的任何
VMA 范围内,所以第三拍 unmap 旧空间时不会把自己干掉。

代码位置:`criu/criu/pie/restorer.c:1220` 和 `:1229` 是两处 `sys_mremap`;
`:1441` 的 `unmap_old_vmas()` 在 `:2340` 被调用,它的参数里同时有 premap 范围和
bootstrap 范围 —— **它要小心跳过这两块。**

第三拍结束后调 `rt_sigreturn`(详见
[05-registers-and-sigframe](05-registers-and-sigframe.md)),这是唯一能一次性
原子设置全部寄存器的机制。

---

## 7. 延伸阅读

- `include/linux/mm_types.h` —— `vm_area_struct` 和 `mm_struct` 的定义
- `mm/mmap.c` 的 `do_mmap()` —— VMA 是怎么被创建的
- `criu/criu/proc_parse.c` 的 `parse_smaps()` —— CRIU 解析 `/proc/PID/smaps` 的
  全过程。**读它能直观感受到「文本接口」带来了多少复杂度**
- `criu/criu/include/vma.h:104` 的 `vma_entry_is_private()`
- `criu/criu/mem.c` —— dump 内存的主逻辑
- [01-process-anatomy](01-process-anatomy.md) —— A/B 二分
- [09-restore-ordering](09-restore-ordering.md) —— 为什么地址空间必须最后换
