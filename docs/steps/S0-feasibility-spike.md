# S0 —— 可行性打靶

**工期:** 1 周 · **前置:** 无 · **产出:** 一张结论表 · **代码去向:** 用完就删

> 相关原理:[08-kernel-module-limits](../principles/08-kernel-module-limits.md)、
> [01-process-anatomy](../principles/01-process-anatomy.md)

---

## 1. 设计思路

### 这一步为什么必须存在

原设计大纲 `02-Kernel-Module-Design-Outline.md` 第五部分的 restore 伪代码用了
`mm_alloc()`、`vm_area_alloc()`、`insert_vm_struct()`、`kthread_create()` 造用户
进程。已经用 grep 核实过:**这些符号在 5.10.29 里大部分没有 `EXPORT_SYMBOL`。**

但 grep 只能告诉你源码里有没有 `EXPORT_SYMBOL` 这一行。实际编译链接还有别的坑:

- `EXPORT_SYMBOL_GPL` vs `EXPORT_SYMBOL` —— 前者要求模块声明 GPL 许可
- 符号导出了,但**头文件不在 `include/linux/` 下**,声明拿不到
- 符号和声明都有,但运行时**上下文不允许**(比如必须持某把锁、不能在原子上下文)
- 结构体字段偏移在不同 `CONFIG_*` 下不同,编译过了但读到垃圾

这四类只有**真编译真加载**才知道。所以 S0 的价值不是代码,是**把「做不到」提前到
第 1 周发现,而不是第三周**。

### 打靶原则

写一个一次性模块,每个待验证项一个 `#ifdef` 开关,逐个打开、编译、加载、看结果。
**不追求正确性,只追求「链接得上 / 跑得起来 / 读到的值对不对」三个 yes/no。**

代码写完就删。不要试图在 S0 的代码上继续长出 A1 —— S0 的代码里会有一堆
`#if 0` 和临时 hack,留着是负债。

---

## 2. CRIU 是怎么做的(参照)

CRIU 有一个对应机制叫 **kerndat**(kernel data),干的是同一件事:启动时探测
当前内核支持什么,把结果缓存起来。

参照文件:`criu/criu/kerndat.c`,以及 `criu/criu/include/kerndat.h` 里的
`struct kerndat_s`。它探测的东西包括:

- `has_dirty_track` —— soft-dirty 能不能用
- `has_memfd` / `has_membarrier_get_registrations`
- `has_pidfd_open` / `has_clone3_set_tid` ← **和我们最相关的一条**
- `task_size` —— 用户空间地址上限

以及 `criu check --all`(入口在 `criu/criu/cr-check.c`)—— 它把每个特性做成一个
独立的 `check_xxx()` 函数,能跑就返回 0。比如 `criu/criu/cr-check.c:943`:

```c
	pid = clone(clone_cb, ca.stack_ptr, CLONE_NEWPID | CLONE_PARENT, &ca);
	if (pid < 0) {
		pr_err("CLONE_PARENT | CLONE_NEWPID don't work together\n");
```

这就是 CRIU 在探测「这个内核版本上 `CLONE_PARENT|CLONE_NEWPID` 能不能一起用」。
**它不看内核版本号,它直接试。** 这个思路要抄过来:S0 不查文档,S0 做实验。

**要抄的:** 每个待验证项独立成函数、返回 yes/no、结果落盘归档。
**不抄的:** CRIU 的 kerndat 有缓存文件和版本协商,S0 不需要。

---

## 3. 待验证项清单

分四组。每组给出**判定标准**和**已知的 grep 结论**(需要实验复核)。

### 组 1:能不能拿到目标 task

| # | 待验证 | 已知 grep 结论 | 实验判定 |
|---|---|---|---|
| 1.1 | `find_get_task_by_vpid()` | **未导出** | 链接失败即确认 |
| 1.2 | `pid_task(find_vpid(nr), PIDTYPE_PID)` | `find_vpid` 待验 | 能拿到非 NULL 且 `->pid` 正确 |
| 1.3 | `get_pid_task()` / `put_task_struct()` | 待验 | 引用计数一增一减,`rmmod` 后无泄漏 |
| 1.4 | RCU 保护是否必需 | —— | 不加 `rcu_read_lock()` 时 lockdep 是否报警 |

1.2 是关键退路。如果 `find_get_task_by_vpid` 拿不到,组合 `find_vpid` +
`get_pid_task` 是等价路径。**必须验证至少一条通**,否则整个 A 轨不成立。

### 组 2:能不能遍历地址空间

| # | 待验证 | 已知 grep 结论 | 实验判定 |
|---|---|---|---|
| 2.1 | `mmget_not_zero()` | **未导出** | 链接失败即确认 |
| 2.2 | `get_task_mm()` / `mmput()` | `mmput` **已导出** | 能拿到 `mm` 且 `mm_users > 0` |
| 2.3 | `mmap_read_lock(mm)` | inline 函数,应可用 | lockdep 无报警 |
| 2.4 | `for (vma = mm->mmap; vma; vma = vma->vm_next)` | 5.10 有 `vm_next` | VMA 数量与 `/proc/PID/maps` 行数一致 |
| 2.5 | `vma->vm_file` → `d_path()` 取路径 | 待验 | 路径与 `/proc/PID/maps` 最后一列一致 |

2.4 的一致性检查是整个 S0 里**最重要的一个实验**,它同时验证了「结构体布局对」
「锁拿对了」「遍历方式对」三件事。

### 组 3:能不能读到页内容

| # | 待验证 | 已知 grep 结论 | 实验判定 |
|---|---|---|---|
| 3.1 | `follow_page()` | **未导出** | 链接失败即确认 |
| 3.2 | `get_user_pages_remote()` | **已导出** | 返回正数,拿到的页内容正确 |
| 3.3 | `access_process_vm()` | **已导出** | 读到的字节与目标进程写入的一致 |
| 3.4 | 读到一个未映射地址会怎样 | —— | 应返回错误,**不应** oops |
| 3.5 | 读一个 `PROT_NONE` 的 guard page | —— | 记录行为 |

3.2 vs 3.3 是一个真实的选型:`access_process_vm` 更简单(直接给 buffer),
`get_user_pages_remote` 更底层(拿到 `struct page *`,后续可以做零拷贝)。
**S0 只需要知道两个都能用,选型放到 A3。**

### 组 4:验证「做不到」的那些

这一组的目的不是找路,是**确认此路不通并留下证据**,免得三个月后有人再试一遍。

| # | 待验证 | 预期结果 |
|---|---|---|
| 4.1 | `mm_alloc()` | 链接失败 |
| 4.2 | `vm_area_alloc()` | 链接失败 |
| 4.3 | `insert_vm_struct()` | 链接失败 |
| 4.4 | `do_mmap()` | 链接失败;`vm_mmap()` **已导出**可作替代 |
| 4.5 | `do_munmap()` | MMU 路径未导出(只有 nommu 导出);`vm_munmap()` 已导出 |
| 4.6 | `alloc_pid()` | 链接失败 → **PID 恢复在内核里做不了** |
| 4.7 | `kernel_execve()` | 链接失败 |
| 4.8 | `vm_insert_page()` | **能链接**,但看第 4 节 |

---

## 4. 4.8 单独说:一个能用但会毁掉语义的符号

`vm_insert_page()` 在 `linux-5.10.29/mm/memory.c:1821` 是真导出的:

```c
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
			struct page *page)
{
	if (addr < vma->vm_start || addr >= vma->vm_end)
		return -EFAULT;
	if (!page_count(page))
		return -EINVAL;
	if (!(vma->vm_flags & VM_MIXEDMAP)) {
		BUG_ON(mmap_read_trylock(vma->vm_mm));
		BUG_ON(vma->vm_flags & VM_PFNMAP);
		vma->vm_flags |= VM_MIXEDMAP;
	}
	return insert_page(vma, addr, page, vma->vm_page_prot);
}
EXPORT_SYMBOL(vm_insert_page);
```

注意 `vma->vm_flags |= VM_MIXEDMAP;` —— 它**强行给 VMA 打上 `VM_MIXEDMAP`**。
这个 flag 的含义是「这个 VMA 里混着有 `struct page` 和没有 `struct page` 的页」,
是给设备驱动映射用的。一个普通的匿名内存 VMA 被打上它之后:

- COW 行为改变
- `get_user_pages` 的快路径失效
- `fork()` 时的页表复制走不同分支
- `/proc/PID/smaps` 的统计出错

所以结论是:**它能链接,但不能用。** 这是原大纲 5.3 节伪代码里最隐蔽的一个错
—— 不是编译不过,是编译过了、跑起来了、然后语义悄悄坏掉。

S0 要做的实验:用它给一个 VMA 插页,然后读 `/proc/PID/smaps` 看统计是否异常,
把结果记进结论表。**这是本步骤唯一一个「实验结果比 grep 结论更有价值」的项。**

---

## 5. 如何测试

S0 的「测试」就是实验本身。但仍然要有明确的通过标准。

### 5.1 目标程序

`tests/progs/known-layout.c` —— 一个地址空间布局完全已知的程序:

```c
/* known-layout.c: a process whose address space we can predict exactly. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* Distinctive byte patterns so the module can prove it read the right page. */
#define MAGIC_A 0xA5
#define MAGIC_B 0x5A

int main(void)
{
	char *anon, *shared;

	/* One private anonymous page, filled with MAGIC_A. */
	anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anon == MAP_FAILED)
		return 1;
	memset(anon, MAGIC_A, 4096);

	/* One shared anonymous page, filled with MAGIC_B. */
	shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return 1;
	memset(shared, MAGIC_B, 4096);

	/* One PROT_NONE guard page for probe 3.5. */
	if (mmap(NULL, 4096, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
		return 1;

	printf("pid=%d anon=%p shared=%p\n", getpid(), anon, shared);
	fflush(stdout);

	for (;;)
		pause();
	return 0;
}
```

用 `MAGIC_A`/`MAGIC_B` 而不是随便填,是为了让「读到的页内容对不对」变成一个
**能自动判定**的问题,而不是靠眼睛看 hexdump。

### 5.2 单项测试流程

对每一项:

```bash
# 1. 只打开这一项
make -C spike PROBE=2.4
# 2. 在 QEMU 里加载
./scripts/run-qemu.sh --script spike/run-one.sh
# 3. 看结果
```

三种结果,分别记录为:

| 结果 | 含义 | 记法 |
|---|---|---|
| 编译/链接失败 | 符号不可用 | `NO-SYMBOL`,附错误信息 |
| 加载后 dmesg 有 WARNING/oops | 能链接但用法错 | `UNSAFE`,附栈回溯 |
| 输出与 `/proc` 一致 | 可用 | `OK` |

**特别注意第二种。** 因为开了 `CONFIG_DEBUG_VM` + `CONFIG_PROVE_LOCKING` +
`CONFIG_DEBUG_ATOMIC_SLEEP`,很多「碰巧没崩」会变成明确的 warning。这三个配置
就是为这一步准备的 —— 没有它们,`UNSAFE` 会被误记成 `OK`。

### 5.3 自动判定脚本

`spike/check.sh`,给 CI 和本地共用:

```sh
#!/bin/sh
# Exit 0 only if the module's report matches /proc for the target process.
set -e

./tests/progs/known-layout > /tmp/kl.out &
sleep 1
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/kl.out)

insmod spike/criu_spike.ko target_pid=$PID
cat /sys/kernel/debug/criu_spike/vmas > /tmp/module-vmas.txt

# The module must see exactly the same number of VMAs as procfs.
proc_n=$(grep -c . /proc/$PID/maps)
mod_n=$(grep -c . /tmp/module-vmas.txt)
echo "procfs VMAs: $proc_n   module VMAs: $mod_n"
[ "$proc_n" = "$mod_n" ] || { echo "MISMATCH"; exit 1; }

# And it must read back the magic bytes we planted.
grep -q 'anon_first_byte=0xa5' /tmp/module-vmas.txt || { echo "BAD ANON"; exit 1; }
grep -q 'shared_first_byte=0x5a' /tmp/module-vmas.txt || { echo "BAD SHARED"; exit 1; }

# Any debug-kernel complaint fails the probe even if the numbers matched.
if dmesg | grep -qE 'WARNING:|BUG:|possible circular locking'; then
	echo "UNSAFE: kernel complained"
	dmesg | tail -40
	exit 1
fi

rmmod criu_spike
echo "SPIKE OK"
```

最后那段 `dmesg | grep` 是重点:**数值对了也可能是 UNSAFE。** 不检查 dmesg 就
把 `UNSAFE` 当 `OK` 记了,是 S0 唯一可能出的严重错误。

---

## 6. 完成标准

- [ ] 组 1-3 的每一项都有 `OK` / `UNSAFE` / `NO-SYMBOL` 结论,附证据
- [ ] 组 1 至少一条路通(否则 A 轨终止)
- [ ] 组 2 的 2.4 一致性检查通过
- [ ] 组 3 的 3.2 或 3.3 至少一条通
- [ ] 组 4 的每一项都确认了预期(此路不通有证据)
- [ ] 结论表写进 `docs/principles/08-kernel-module-limits.md` 的「实验复核」一节
- [ ] spike 代码删除,或移入 `spike/` 并在 README 标注「一次性代码,勿复用」

## 7. 如果 S0 的结论是「不行」

组 1 或组 2 全部失败时,A 轨不成立。此时的选项,按推荐度:

1. **只做 B 轨**(用户空间 mini-restore)—— 学习价值最高的部分其实在这边
2. **改成内核补丁而不是模块** —— 补丁能加自己的 `EXPORT_SYMBOL`,但从此需要
   自编译内核,而且失去了「模块可插拔」这个唯一的部署优势
3. **只做 X 轨** —— 给 CRIU 补 `start_time` 那个洞,范围小但是真实贡献

**这三个选项都是好结果。** S0 花一周证明某条路不通,比花三个月证明它不通,便宜
十二倍。
