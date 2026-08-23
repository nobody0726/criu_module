# B1 —— 用户态迷你 restore(单进程)

**工期:** 3-4 周 · **前置:** 无(与 A 轨并行) · **产出:** 能恢复真 criu 产出的极简进程镜像

> 相关原理:[04-image-format](../principles/04-image-format.md)、
> [05-registers-and-sigframe](../principles/05-registers-and-sigframe.md)、
> [09-restore-ordering](../principles/09-restore-ordering.md)

---

## 1. 设计思路

### 为什么 restore 留在用户态

这是整个计划里最重要的一个架构决定,理由已经在
[08-kernel-module-limits](../principles/08-kernel-module-limits.md) 第 5 节里核实过:
**restore 的核心动作必须由「即将成为目标进程的那个 task」自己执行。**

关键约束:
- `clone3(set_tid)` 要求调用者在目标 pid namespace 里,而且新 task 的父亲就是调用者
- 地址空间替换必须由 `current` 自己做 —— 换完之后原来那段代码就不在了
- `rt_sigreturn` 是**唯一**能一次性原子设置全部寄存器的机制,它只能作用于 `current`

内核模块能做的最多是「代替某个 task 做事」,但它没法**成为**那个 task。
`kthread_create` 出来的线程是内核线程,没有 `mm`,而且它的父亲是 `kthreadd`,
不可能变成用户进程树里的一个节点。

**所以 B 轨的存在不是妥协,是唯一正确的形状。** 真正的 CRIU 也是这么分的:
它的 restore 端也是用户态代码(`criu/criu/pie/restorer.c`),只是它同时把 dump 也
放在用户态。我们把 dump 挪进内核,restore 留在原处。

### B 轨的验证器是真 criu 的镜像

和 A 轨的 oracle 反向使用配对:

```
A 轨:  我们的 dump  ──► 镜像 ──► 真 criu restore    (验证 dump)
B 轨:  真 criu dump ──► 镜像 ──► 我们的 restore     (验证 restore)
最终:  我们的 dump  ──► 镜像 ──► 我们的 restore     (两侧都已各自验证过)
```

**这是整个计划的结构性运气所在**:因为 dump 和 restore 之间的接口是磁盘上的文件
而不是函数调用,两侧可以完全独立开发,各自被对方的成熟实现验证。等两侧都过了,
把它们接起来几乎不会出新问题。

### restore 的形状:一条不可逆动作的流水线

restore 和 dump 在结构上完全不对称。dump 是纯读,顺序随意、可重试、可并行。
restore 是一串**不可逆的一次性阀门**,顺序几乎完全被约束死。

**支配一切的原则:不可逆的动作尽可能晚做;做之前它需要的一切必须已经就位。**

而唯一一条强 A/B 顺序约束是:

> **A 类恢复(地址空间替换)必须在所有 B 类恢复之后。**

因为地址空间一换,restore 程序自己的代码和栈就不在了。换完之后你唯一能做的事情
就是 `rt_sigreturn` 跳进目标进程。所以:

```
1. 读镜像、校验、分配数据结构        ← 可逆,随便做
2. clone3(set_tid) 建 task           ← 不可逆(pid 用掉了)
3. 恢复 B 类:fd、信号、定时器、creds ← 大部分不可逆
4. premap 目标地址空间到临时位置      ← 可逆
5. 把恢复参数 + sigframe 搬进目标空间  ← 最后一次能用自己的内存
6. mremap 到最终地址 / unmap 旧的     ← 单向阀门,自己的代码消失
7. rt_sigreturn                     ← 不返回
```

**第 5 步和第 6 步之间是整个 restore 的悬崖。** 过了 6,任何错误都是段错误,
没有日志、没有错误码。所以所有可能失败的事情必须在 6 之前做完。

### 1.4 premap → mremap 这个手法

问题:目标进程的 VMA 要恢复到原来的虚拟地址,但那些地址现在可能被 restore 程序
自己占着(它自己的代码、栈、libc 都在某处)。

不能先 unmap 自己再 map 目标 —— unmap 自己之后就没有代码继续执行了。

CRIU 的解法分三拍:

```
第一拍 premap:  把目标的所有 VMA 先 mmap 到一块空闲的临时区域
                (`premmapped_addr`),内容填好。此时自己的地址空间还完整。

第二拍 搬家:    把 restore 参数、sigframe、以及执行第三拍所需的那一小段代码
                (`restorer` blob)搬到一块两边都不冲突的区域(`bootstrap`)。
                然后跳到那段代码上执行。

第三拍 mremap:  在 bootstrap 里,先 unmap 旧的一切(包括原来的 libc 和栈),
                再把 premap 区域 mremap 到最终地址。mremap 是原子的,
                不存在「中间态没有内存」的窗口。
```

关键在于 **`mremap` 而不是 `memcpy`**:`mremap` 只改页表项,不搬数据,而且它可以
把一段映射移到任意目标地址。这让「内容已经就位、地址还不对」变成一个纯页表操作。

`bootstrap` 区域的作用是给第三拍提供一个「站得住的地方」——它不在目标进程的任何
VMA 范围内,所以第三拍 unmap 旧空间时不会把自己 unmap 掉。最后一步由
`rt_sigreturn` 完成后,`bootstrap` 也被 unmap(见 `restorer.c:1407`)。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 主流程入口

`criu/criu/cr-restore.c:641` 的 `restore_one_alive_task()` 是单个存活任务的恢复
主线。读它的顺序就是 restore 的顺序,这是 B1 最值得逐行读的一个函数。

`criu/criu/cr-restore.c:1097` 的 `fork_with_pid()` 是建 task 的地方。里面有两处
值得注意:

```c
	BUG_ON(ca.clone_flags & CLONE_VM);		/* line 1189 */
```

—— CRIU 明确不支持 `CLONE_VM` 而非 `CLONE_THREAD` 的进程(这正是 A8 用例 11
要报 `-EOPNOTSUPP` 的依据)。

以及 `criu/criu/cr-restore.c:1552` 附近的自检:

```c
	pid = getpid();
	if (vpid(current) != pid) {
		pr_err("Pid %d do not match expected %d\n", pid, vpid(current));
		set_task_cr_err(EEXIST);
		goto err;
	}
```

**pid 是被断言的,不是尽力而为的。** 这条断言是「载体全新,内容全等」在代码里的
直接体现:新 `task_struct` 是全新分配的,但它的 vpid 必须精确等于原来的值,
不等就失败退出。

### 2.2 `clone3` + `set_tid`

`criu/criu/clone-noasan.c:55-84`:

```c
	c_args.flags = flags;
	c_args.set_tid = ptr_to_u64(&pid);
	c_args.set_tid_size = 1;
	pid = syscall(__NR_clone3, &c_args, sizeof(c_args));
```

**`set_tid_size = 1` 是关键。** `set_tid` 是一个数组,可以为嵌套的每一层 pid
namespace 各指定一个 pid。`size = 1` 意味着只钉住**最内层**那个 namespace 的
pid —— 也就是进程自己看到的那个 pid。

外层(比如宿主机)的 pid 由内核自由分配。**这正是容器迁移能成立的原因**:
容器内的 pid 必须一样,宿主机的 pid 无所谓,也不可能一样(目标机器上那个号
可能已经被占了)。

`set_tid` 需要 `CAP_SYS_ADMIN`(在目标 pid namespace 里),这是 restore 需要
特权的主要原因之一。

### 2.3 premap / bootstrap / mremap 的代码位置

- `restorer.c:1220` 和 `restorer.c:1229` —— 两处 `sys_mremap`,带
  `MREMAP_MAYMOVE | MREMAP_FIXED`,这是第三拍的核心
- `restorer.c:1402-1403` —— `bootstrap_start` / `bootstrap_len` 全局变量
- `restorer.c:2264-2265` —— 从 `args` 里取出 bootstrap 范围
- `restorer.c:1441` 的 `unmap_old_vmas()` —— 在 `restorer.c:2340` 被调用,
  负责第三拍开头的「unmap 旧的一切」。注意它的参数里同时有 `premmapped` 范围和
  `bootstrap` 范围:**它要小心地跳过这两块,否则会把自己干掉**
- `restorer.c:1407` —— `sys_munmap(bootstrap_start, bootstrap_len - vdso_rt_size)`,
  收尾时释放 bootstrap 自己

### 2.4 `rt_sigreturn`:唯一能一次设全部寄存器的机制

`restorer.c:681`:

```c
static void noinline rst_sigreturn(unsigned long new_sp, struct rt_sigframe *sigframe)
{
	ARCH_RT_SIGRETURN_RST(new_sp, sigframe);
}
```

原理:内核在投递信号时,会把当前所有寄存器(通用寄存器、段寄存器、FPU 状态、
信号掩码)打包成一个 `struct rt_sigframe` 放到用户栈上。handler 返回时调
`rt_sigreturn`,内核**从栈上那个结构里把所有寄存器恢复回去**。

CRIU 反过来用它:**手工在栈上构造一个 `rt_sigframe`,填进目标进程 checkpoint 时
的寄存器值,然后直接调 `rt_sigreturn`。** 内核不知道这不是真的信号返回,它照常
把那些值加载进寄存器,然后「返回」到 `rip` 指向的位置 —— 也就是目标进程被
checkpoint 的那条指令。

没有别的办法能做到这件事。逐个设寄存器是不行的,因为设 `rip` 的那一刻就跳走了,
后面的寄存器再也没机会设。**`rt_sigreturn` 的原子性是 restore 能成立的前提。**

参照 `criu/criu/sigframe.c` 和 `criu/criu/arch/x86/sigframe.c` 看 sigframe
怎么填。

### 2.5 B1 抄多少

**要抄的:** 整个流程骨架、premap/mremap 三拍、sigframe 构造、`clone3` 用法。
**允许直接复用的:** `criu/images/*.pb-c.[ch]`(protobuf 生成代码)、
`criu/compel/` 里的 sigframe 定义。**这是用户态代码,没有内核态的限制,
直接链接 CRIU 的库比重写更明智。**

**不抄的:** `pie/` 那套 position-independent 的构建魔法。CRIU 把 restorer 编译成
一个完全不依赖 libc、位置无关的 blob,是因为它要在目标进程的地址空间里执行。
B1 的极简版可以走一条捷径:见 4.1。

---

## 3. 文件结构

**Create:**
- `userspace/mini-restore/main.c` —— 命令行,读镜像目录
- `userspace/mini-restore/img_read.c` —— 镜像读取(magic + 长度前缀 + protobuf)
- `userspace/mini-restore/task_create.c` —— `clone3(set_tid)`
- `userspace/mini-restore/rst_mem.c` —— premap + 页内容填充
- `userspace/mini-restore/rst_files.c` —— fd 恢复(只做常规文件)
- `userspace/mini-restore/sigframe.c` —— sigframe 构造
- `userspace/mini-restore/restorer.S` —— 第三拍的汇编部分
- `userspace/mini-restore/Makefile`
- `tests/b1-restore.sh`

**Interfaces produced:**

```c
/* One restore session. Mirrors the image set, read up front so that every
 * failure that can be detected happens before the first irreversible step. */
struct rst_ctx {
	char		*img_dir;
	pid_t		 target_pid;
	CoreEntry	*core;
	MmEntry		*mm;
	int		 pages_fd;		/* pages-N.img, kept open */
	PagemapEntry	**pagemaps;
	unsigned	 nr_pagemaps;
	void		*premmapped_addr;	/* where VMAs land first */
	unsigned long	 premmapped_len;
	void		*bootstrap_start;	/* where stage-3 code lives */
	unsigned long	 bootstrap_len;
};

/* Phase 1: read and validate everything. No side effects on the system, so a
 * failure here costs nothing. */
int rst_read_images(const char *img_dir, struct rst_ctx *ctx);
int rst_validate(struct rst_ctx *ctx);

/* Phase 2: create the task with the exact pid from the image. Irreversible. */
pid_t rst_clone_with_pid(pid_t pid, int (*fn)(void *), void *arg);

/* Phase 3: B-class restore, in the child. */
int rst_restore_files(struct rst_ctx *ctx);
int rst_restore_creds(struct rst_ctx *ctx);
int rst_restore_fs(struct rst_ctx *ctx);

/* Phase 4: premap the target address space to a temporary location and fill
 * page content. Still reversible: nothing of ours is unmapped yet. */
int rst_premap_vmas(struct rst_ctx *ctx);

/* Phase 5+6+7: the point of no return. Moves to bootstrap, unmaps the old
 * address space, mremaps premapped VMAs into place, and rt_sigreturns into
 * the target. Never returns. */
void rst_finalize(struct rst_ctx *ctx) __attribute__((noreturn));
```

`rst_finalize` 标 `noreturn` 不只是给编译器看的:**它是这个 API 最重要的一条
文档。** 调用它之后没有错误处理的机会,所以调用点之前必须已经没有任何可能失败的
工作剩下。

---

## 4. 关键实现要点

### 4.1 极简版可以绕开 PIE blob

CRIU 的 `pie/` 目录里那套构建魔法(编译成位置无关、不依赖 libc 的 blob,再
`memcpy` 进目标地址空间)是为了通用性。B1 有一条更简单的路:

**把第三拍的代码放在 `bootstrap` 区域,用汇编写,不调用任何函数。**

```
第三拍需要做的全部事情:
  1. munmap 旧地址空间的若干区间   ← 直接 syscall
  2. mremap premap 区域到最终地址   ← 直接 syscall
  3. munmap bootstrap 自己(除了 sigframe 所在页)
  4. rt_sigreturn                  ← 直接 syscall
```

四个系统调用,没有循环之外的控制流,不需要任何库函数。用一段手写汇编 + 一个
参数结构体就能完成,不需要 CRIU 那套 PIE 构建体系。

**代价:** VMA 数量多时第 1、2 步要循环,汇编里写循环容易出错。缓解:参数结构体
里预先算好所有要 munmap 和 mremap 的区间(在第四拍用 C 算),汇编只负责按数组
逐个发 syscall。**把逻辑留在 C 里,汇编只做搬砖,是这里的正确分工。**

### 4.2 pages 文件的读取要用 `pread`,不要 `mmap`

直觉上 `mmap` pages 文件再 `memcpy` 更快。但:

```c
	/* Read page content with pread, not mmap. An mmap of the image file
	 * would itself occupy address space in the region we are about to
	 * unmap, and mapping it MAP_PRIVATE would double our RSS.
	 */
	ret = pread(ctx->pages_fd, dst, len, off);
```

理由是地址空间的洁癖:第三拍要 unmap 一切,任何多出来的映射都是一个要小心
处理的例外。**用 `pread` 直接写进 premap 好的目标页里,不引入新映射。**

### 4.3 pagemap 的偏移单位

A3 里已经踩过这个坑,B1 是它的另一面:

```c
	/* pagemap offsets are in PAGES. The first record in the file is a
	 * pagemap_head carrying pages_id, not a real mapping.
	 */
	off = (loff_t)page_index * PAGE_SIZE;
```

**B1 和 A3 在这里必须完全一致。** 一个建议:把「pagemap 偏移换算」写成一个
共享的头文件里的 `static inline`,A3(内核)和 B1(用户态)都用它。同一个错误
犯两次的概率远小于两处独立实现不一致的概率。

### 4.4 premap 区域选址

`premmapped_addr` 必须满足:
- 足够大,装下目标进程所有 VMA 的总长度
- 不与目标进程的任何 VMA 的**最终**地址重叠(否则第三拍 mremap 时自己踩自己)
- 不与 restore 程序自己的映射重叠

做法:算出目标进程 VMA 的地址上界,在它之上找一块空闲区域。或者更简单:
用 `mmap(NULL, total_len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`
让内核选一块,然后**检查内核选的位置是否与目标 VMA 冲突**,冲突就换 `MAP_FIXED`
到一个手算的地址。

```c
	/* Let the kernel pick, then verify. A collision with a target VMA would
	 * make stage 3 mremap over itself, which corrupts silently rather than
	 * failing loudly -- so check explicitly.
	 */
	if (region_overlaps_any_target_vma(ctx, addr, total_len)) {
		munmap(addr, total_len);
		addr = mmap(pick_above_target(ctx), total_len, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
			    MAP_FIXED, -1, 0);
	}
```

**这个检查必须显式写。** 冲突的症状是静默的内存损坏,不是崩溃 —— 是最难调的
那一类 bug。

### 4.5 vDSO

目标进程的 vDSO 地址和当前内核给出的 vDSO 地址可能不同。CRIU 做符号级重定位
(`criu/criu/pie/util-vdso.c`)。

**B1 的处理:检测两者是否一致,不一致就明确报错退出。**

```c
	/* If the target's vDSO does not sit where this kernel puts it, a proper
	 * restore needs symbol-level relocation (see criu's pie/util-vdso.c).
	 * B1 does not implement that: fail loudly instead of producing a process
	 * that segfaults the first time it calls gettimeofday().
	 */
	if (target_vdso_start != current_vdso_start) {
		fprintf(stderr, "vDSO moved (%lx -> %lx); B1 cannot relocate\n",
			target_vdso_start, current_vdso_start);
		return -EOPNOTSUPP;
	}
```

因为 B1 的测试是「同一台机器、同一个内核、dump 后立刻 restore」,vDSO 地址
实际上会一致。**这个限制在 B1 的范围内不影响任何测试,但必须显式检测** —— 
不检测的话,当条件变了(换内核、跨机器),症状是目标进程在第一次调
`gettimeofday()` 时段错误,几乎无法定位。

### 4.6 sigframe 里的 FPU 状态

`rt_sigframe` 里的 FPU 部分(`xsave` 区域)有严格的格式要求,`xstate_bv` 位图
必须与实际存的内容一致,而且大小取决于 CPU 支持的特性。

**照抄 CRIU 的做法,不要自己算。** 复用 `criu/criu/arch/x86/sigframe.c` 和
`criu/compel/arch/x86/` 里的定义。填错的症状是 `rt_sigreturn` 返回 `-EFAULT`,
或者更坏,成功了但浮点寄存器是垃圾。

---

## 5. 如何测试

### 5.1 B1 的验收脚本

```sh
#!/bin/sh
# B1 acceptance: our restore must handle images produced by the REAL criu.
# This is the mirror image of A3's gate.
set -e

IMGS=/tmp/b1-imgs
rm -rf $IMGS && mkdir -p $IMGS

./tests/progs/minimal > /tmp/b1.out 2>&1 &
sleep 3
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/b1.out)
LAST_TICK=$(grep -o 'tick=[0-9]*' /tmp/b1.out | tail -1 | cut -d= -f2)

# --- dump with the REAL criu: the images are known-good by construction ---
criu dump -t $PID -D $IMGS -v4 --shell-job
# The process is gone now (no --leave-running), so its pid is free.

# --- restore with OUR implementation ---
./userspace/mini-restore/mini-restore -D $IMGS >> /tmp/b1.out 2>&1 &
sleep 3

kill -0 $PID || { echo "FAIL: restored process not alive"; exit 1; }

NEW_TICK=$(grep -o 'tick=[0-9]*' /tmp/b1.out | tail -1 | cut -d= -f2)
[ "$NEW_TICK" -gt "$LAST_TICK" ] || {
	echo "FAIL: not continuing ($LAST_TICK -> $NEW_TICK)"; exit 1; }

if grep -qE 'HEAP CORRUPT|STACK CORRUPT' /tmp/b1.out; then
	echo "FAIL: restored process reports memory corruption"
	exit 1
fi

kill -9 $PID 2>/dev/null || true
echo "B1 OK -- we can restore real criu images"
```

**注意这里 dump 不加 `--leave-running`。** B1 必须让原进程消失,否则 pid 被占着,
`clone3(set_tid)` 会返回 `EEXIST`。这与 A3 的脚本不同(A3 要 `--leave-running`
才能对比),是两个方向的测试在流程上的一个实际差别。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 恢复真 criu 产出的极简进程镜像 | 进程存活 |
| 2 | 恢复后 pid 与镜像里一致 | `kill -0 $PID` 成功 |
| 3 | 从 checkpoint 点继续执行 | `tick` 递增不归零 |
| 4 | 堆内容完整 | 无 `HEAP CORRUPT` |
| 5 | 栈内容完整 | 无 `STACK CORRUPT` |
| 6 | fd 0/1/2 可用 | 恢复后输出仍可见 |
| 7 | 恢复后 `getpid()` 返回正确值 | 进程自己打印的 pid 一致 |
| 8 | 目标 pid 已被占用 | 明确报 `EEXIST` 并退出,**不损坏占用者** |
| 9 | 镜像目录缺文件 | 在任何不可逆动作**之前**报错退出 |
| 10 | 镜像 magic 错误 | 同上 |
| 11 | 镜像版本不匹配 | 同上 |
| 12 | 200 个 VMA 的进程 | 恢复成功(第三拍循环正确) |
| 13 | 恢复后进程能正常 `exit`,父进程能 `wait` | 退出码可取 |
| 14 | 恢复一个 100MB RSS 的进程 | 成功,内存内容抽样一致 |
| 15 | 恢复后 `/proc/$PID/maps` 与镜像里的 VMA 列表一致 | `diff` 通过 |
| 16 | vDSO 地址不一致的构造场景 | 明确 `-EOPNOTSUPP`,不产出会崩的进程 |

**用例 9、10、11 是一组,它们检验的是同一件事:「所有能检测的失败,都在
`clone3` 之前检测出来」。** 这是 B1 最重要的设计约束,也是最容易违反的 —— 
一边读镜像一边干活是自然的写法,但它会导致「pid 已经用掉了才发现镜像坏了」。

判定方法:在 `rst_clone_with_pid` 里加一个断言,确认所有镜像已经读完:

```c
	/* Everything readable must already be read: this is the last point at
	 * which failing is free. */
	assert(ctx->core && ctx->mm && ctx->pagemaps && ctx->pages_fd >= 0);
```

用例 15 的价值:它不依赖目标进程自己的自检,是一个外部的、结构化的比对。
构造:

```sh
# The restored address space must match the image, VMA for VMA.
crit decode -i $IMGS/mm-$PID.img --pretty \
	| sed -n 's/.*"start": "\(0x[0-9a-f]*\)".*/\1/p' | sort > /tmp/want
sed -n 's/^\([0-9a-f]*\)-.*/0x\1/p' /proc/$PID/maps | sort > /tmp/got
diff /tmp/want /tmp/got
```

### 5.3 ZDTM 增量

B1 也能接 ZDTM,方向相反:用真 criu dump、我们的 restore。

```sh
# Drive zdtm with the real criu for dump and our mini-restore for restore, by
# pointing --criu-bin at a shim that dispatches the other way from A3's.
sudo ./criu/test/zdtm.py run -t zdtm/static/env00 \
	--criu-bin $PWD/userspace/restore-shim/restore-shim
```

`restore-shim` 与 A3 的 `criu-shim` 互为镜像:

| 子命令 | `criu-shim`(A3) | `restore-shim`(B1) |
|---|---|---|
| `dump` | 我们的内核模块 | exec 真 criu |
| `restore` | exec 真 criu | 我们的 mini-restore |
| 其他 | exec 真 criu | exec 真 criu |

**两个 shim 加起来,让 489 个 ZDTM 测试同时成为两条轨的测试集。** 这是把
CRIU 当 oracle 这个思路能给出的最大回报。

初期能过的会很少(B1 只支持极简进程),但 `ci/zdtm-restore-allowlist.txt`
的行数同样是 B 轨的进度仪表盘。

---

## 6. 完成标准

- [ ] 16 个用例通过,含 8、9、16 三个错误路径用例
- [ ] `rst_clone_with_pid` 之前的断言确认所有镜像已读完
- [ ] `rst_finalize` 之前无任何可能失败的工作(代码审查逐行确认)
- [ ] pagemap 偏移换算与 A3 用同一个 `static inline`
- [ ] premap 区域与目标 VMA 的冲突检查已实现
- [ ] `ci/zdtm-restore-allowlist.txt` 至少 3 个测试
- [ ] 已知限制写进本文件附录:vDSO 不重定位、单线程、无 socket
