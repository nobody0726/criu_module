# A1 —— 只读探针 + 对照 diff

**工期:** 1-2 周 · **前置:** S0 · **产出:** 能读出目标进程 VMA 列表,与 `/proc` 逐字段一致

> 相关原理:[01-process-anatomy](../principles/01-process-anatomy.md)、
> [03-memory-and-vma](../principles/03-memory-and-vma.md)

---

## 1. 设计思路

### 边界:这一步刻意不做什么

不冻结、不序列化、不写镜像文件、不 restore。只做一件事:**从内核里读出目标进程的
地址空间描述,通过 debugfs 打印出来。**

这个边界是刻意的。A1 的产出物是**读取层**,它会被 A3 到 A8 全部复用。让它单独成
一步,是为了在没有任何其他复杂度干扰的情况下把它做对。

### 为什么先做内存而不是 fd

三个原因:

1. **内存是 dump 的大头。** 镜像里 99% 的字节是页内容。
2. **它有最好的对照物。** `/proc/PID/maps` 和 `/proc/PID/smaps` 提供逐字段可比
   的参照,fd 那边 `/proc/PID/fdinfo/` 信息稀疏得多。
3. **它最危险。** mm 是整个内核里锁最复杂的子系统之一。早点撞上锁问题比晚撞好。

### 不冻结会读到什么

会读到**撕裂的状态**:遍历到一半目标进程 `mmap` 了新区域,或者 `munmap` 掉了你
正要读的那个 VMA。这是真实风险,不是理论风险。

A1 的应对不是「冻结」(那是 A2),而是:

- 目标程序 `known-layout.c` 在打印完地址后进 `pause()`,**自己不再动地址空间**
- 全程持 `mmap_read_lock(mm)`,防止**别人**改
- 读完立刻 `mmput()`

这样 A1 就能在不实现冻结的前提下拿到稳定结果。**把「稳定性」这个需求从「冻结机制」
里剥离出来,是 A1 能独立成步的关键。**

### 输出格式:刻意模仿 `/proc/PID/maps`

debugfs 输出格式做成和 `/proc/PID/maps` **逐字段对齐**,这样对照 diff 可以直接
`diff <(cat probe) <(cat maps)`,不需要写解析器。

```
7f8e4c021000-7f8e4c022000 rw-p 00000000 00:00 0                          [anon]
```

同时输出一份扩展信息(页内容首字节、`vm_flags` 原始值),放在另一个 debugfs 文件里,
不污染可 diff 的那份。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 CRIU 从哪里读 VMA

参照文件:`criu/criu/proc_parse.c`,函数 `parse_smaps()`。

CRIU **解析 `/proc/PID/smaps` 文本**。它必须这样做,因为它在用户空间,没有别的
途径。这就是本项目最主要的性能论点:内核模块直接读 `mm->mmap` 链表,**省掉了内核
格式化成文本 + 用户空间解析回结构体这一整个来回**。

值得注意的是 CRIU 为此付出的代价 —— `parse_smaps()` 要处理:

- `maps` 与 `smaps` 的字段差异
- VMA 是不是 vDSO / vvar / vsyscall(靠**匹配路径名字符串**)
- `[heap]` `[stack]` 这些伪路径
- 内核不同版本的 smaps 字段增减

这些**全部**是文本解析引入的复杂度,不是问题本身固有的。内核模块里 `vma->vm_flags`
就是个 `unsigned long`,不需要把 `rw-p` 解析回 bit。

### 2.2 CRIU 的四类 VMA

`criu/criu/include/image.h:91`:

```c
#define VMA_FILE_PRIVATE (1 << 6)
#define VMA_FILE_SHARED	 (1 << 7)
#define VMA_ANON_SHARED	 (1 << 8)
#define VMA_ANON_PRIVATE (1 << 9)
```

这是 `mmap` 两个正交维度(有无文件 × 私有/共享)的叉乘。A1 就要开始按这四类
归类,因为**分类错了后面全错**:

| 类 | 内核里怎么判定 |
|---|---|
| `VMA_FILE_PRIVATE` | `vma->vm_file != NULL && !(vm_flags & VM_SHARED)` |
| `VMA_FILE_SHARED` | `vma->vm_file != NULL && (vm_flags & VM_SHARED)` |
| `VMA_ANON_SHARED` | `vma->vm_file == NULL && (vm_flags & VM_SHARED)` |
| `VMA_ANON_PRIVATE` | `vma->vm_file == NULL && !(vm_flags & VM_SHARED)` |

注意 `VMA_ANON_SHARED` 在内核里其实**也有 `vm_file`** —— 匿名共享内存底下是
一个 shmem 文件。所以判定要用 `vma_is_anonymous(vma)` 或检查
`vma->vm_ops == &shmem_vm_ops`,不能只看 `vm_file` 是否为 NULL。
**这是 A1 最容易出的分类错误,S0 的 `known-layout.c` 里那个 `MAP_SHARED|MAP_ANONYMOUS`
页就是专门用来抓它的。**

### 2.3 CRIU 判断哪些页要 dump

`criu/criu/mem.c:105-175`,`should_dump_page()`。A1 不实现它,但要**读懂它**,
因为 A3 要用。它跳过:

- guard page(`PROT_NONE`)
- `VMA_FILE_PRIVATE && (pme & PME_FILE)` —— 注释写的是「给还没被 COW 的私有映射
  页做的优化」:内容和磁盘一致,重新 `mmap` 就有,存它是浪费
- 不存在的页、全零页

**要抄的:** 四类分类法、`should_dump_page` 的跳过逻辑。
**不抄的:** 一切文本解析。

---

## 3. 文件结构

**Create:**
- `kernel_module/Makefile`
- `kernel_module/core/main.c` —— 模块入口、`MODULE_LICENSE("GPL")`、debugfs 注册
- `kernel_module/core/probe.c` —— 目标 task 查找、引用计数
- `kernel_module/checkpoint/vma_walk.c` —— VMA 遍历与分类
- `kernel_module/include/criu_kernel.h` —— 公共结构与原型
- `tests/progs/known-layout.c` —— 见 S0(A1 复用)
- `tests/compare/diff-maps.sh` —— 对照 diff 脚本

**Interfaces produced**(A2-A8 依赖这些):

```c
/* probe.c: resolve a vpid to a pinned task_struct. Caller must
 * put_task_struct(). Returns NULL if no such task. */
struct task_struct *criu_get_task(pid_t vpid);

/* vma_walk.c: classification matching CRIU's VMA_* status bits. */
enum criu_vma_class {
	CRIU_VMA_FILE_PRIVATE,
	CRIU_VMA_FILE_SHARED,
	CRIU_VMA_ANON_SHARED,
	CRIU_VMA_ANON_PRIVATE,
};

enum criu_vma_class criu_classify_vma(struct vm_area_struct *vma);

/* Callback invoked once per VMA with mmap_read_lock held. Returning
 * non-zero aborts the walk and is propagated to the caller. */
typedef int (*criu_vma_fn)(struct vm_area_struct *vma, void *arg);

int criu_walk_vmas(struct task_struct *task, criu_vma_fn fn, void *arg);
```

`criu_walk_vmas` 用回调而不是返回数组,是因为**回调期间锁是持着的**,把锁的
生命周期关在一个函数里比让调用者管更安全。这个接口形状决定了 A3 的写镜像逻辑
必须是流式的 —— 这是好事,大进程的 VMA 列表不该全部驻留。

---

## 4. 关键实现要点

### 4.1 拿 task 的正确姿势

```c
struct task_struct *criu_get_task(pid_t vpid)
{
	struct task_struct *task;

	rcu_read_lock();
	task = pid_task(find_vpid(vpid), PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}
```

三个要点:

1. `find_vpid` + `pid_task` 必须在 `rcu_read_lock()` 里 —— 否则 task 可能在你
   拿到指针后就被释放。开了 `CONFIG_PROVE_RCU` 会直接报警。
2. `get_task_struct()` 必须在**解锁之前**调 —— 拿到引用计数才算安全。
3. `find_get_task_by_vpid()` 是做同样事的封装,但 S0 已确认它**未导出**。

### 4.2 拿 mm 的正确姿势

```c
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;	/* kernel thread, or already exited */

	mmap_read_lock(mm);
	/* ... walk ... */
	mmap_read_unlock(mm);
	mmput(mm);
```

`get_task_mm()` 返回 NULL 有两种情况:目标是内核线程(本来就没有 mm),或者
已经在 `exit_mm()` 之后。两种都该返回 `-ESRCH`,不该 oops。

**注意 5.8 之后 `mm->mmap_sem` 改名为 `mm->mmap_lock`,访问要用
`mmap_read_lock()` 这套 helper。** 直接 `down_read(&mm->mmap_sem)` 在 5.10 上
编译不过 —— `lint.yml` 里有一条专门拦这个。

### 4.3 分类函数

```c
enum criu_vma_class criu_classify_vma(struct vm_area_struct *vma)
{
	bool shared = !!(vma->vm_flags & VM_SHARED);

	/* An anonymous shared mapping still has a vm_file -- shmem creates one
	 * behind the scenes -- so vm_file alone cannot tell the two apart.
	 */
	if (vma_is_anonymous(vma) || (shared && vma_is_shmem(vma)))
		return shared ? CRIU_VMA_ANON_SHARED : CRIU_VMA_ANON_PRIVATE;

	return shared ? CRIU_VMA_FILE_SHARED : CRIU_VMA_FILE_PRIVATE;
}
```

S0 要顺手验证 `vma_is_shmem()` 在 5.10.29 里可用(它在 `include/linux/mm.h`,
但依赖 `CONFIG_SHMEM`)。不可用时的退路是比较
`vma->vm_ops == &shmem_vm_ops` —— 但 `shmem_vm_ops` 未导出,所以退路是
检查 `vma->vm_file->f_inode->i_sb->s_magic == TMPFS_MAGIC`。

### 4.4 取路径

```c
	if (vma->vm_file) {
		char *p = d_path(&vma->vm_file->f_path, buf, buflen);
		if (IS_ERR(p))
			p = "<unknown>";
		/* d_path fills the buffer from the END, so p points into the
		 * middle of buf. Never assume p == buf.
		 */
	}
```

`d_path()` 从缓冲区**尾部**往前填,返回值指向缓冲区中间。这是内核里一个经典的
新手陷阱 —— 用 `buf` 而不是返回值,会打印出垃圾。

---

## 5. 如何测试

### 5.1 手段:对照 diff(强度弱,但极早)

`tests/compare/diff-maps.sh`:

```sh
#!/bin/sh
# A1's acceptance test: the module's view of the address space must match
# procfs exactly. Any diff is a bug in our walk, since both read the same mm.
set -e

./tests/progs/known-layout > /tmp/kl.out &
TARGET=$!
sleep 1
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/kl.out)

insmod kernel_module/criu_kernel.ko
echo "$PID" > /sys/kernel/debug/criu/target

# Normalise both sides to "start-end perms offset dev inode path".
norm() { awk '{ printf "%s %s %s %s %s %s\n", $1,$2,$3,$4,$5,($6==""?"-":$6) }' "$1"; }

norm /proc/$PID/maps                      > /tmp/a.txt
norm /sys/kernel/debug/criu/maps          > /tmp/b.txt

if ! diff -u /tmp/a.txt /tmp/b.txt; then
	echo "FAIL: module view differs from procfs"
	kill $TARGET; rmmod criu_kernel
	exit 1
fi

# Classification: known-layout.c plants exactly one of each interesting class.
grep -q 'class=ANON_PRIVATE' /sys/kernel/debug/criu/vmas_ext
grep -q 'class=ANON_SHARED'  /sys/kernel/debug/criu/vmas_ext
grep -q 'class=FILE_PRIVATE' /sys/kernel/debug/criu/vmas_ext

# Page content: proves we read the right physical page, not just metadata.
grep -q 'anon_first_byte=0xa5'   /sys/kernel/debug/criu/vmas_ext
grep -q 'shared_first_byte=0x5a' /sys/kernel/debug/criu/vmas_ext

# A clean numeric match with a dirty dmesg is still a failure.
if dmesg | grep -qE 'WARNING:|BUG:|possible circular locking|sleeping function'; then
	echo "FAIL: kernel complained"
	dmesg | tail -40
	exit 1
fi

kill $TARGET
rmmod criu_kernel
echo "A1 OK"
```

### 5.2 必须覆盖的测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | `known-layout` 的四类 VMA 全部正确分类 | `vmas_ext` 里各类都出现 |
| 2 | VMA 数量与行数与 `/proc/PID/maps` 完全一致 | `diff` 为空 |
| 3 | 页内容首字节 = 植入的 magic | grep 命中 |
| 4 | 目标 pid 不存在 | 返回 `-ESRCH`,不 oops |
| 5 | 目标是内核线程(如 pid 2 = kthreadd) | 返回 `-ESRCH`,不 oops |
| 6 | 目标进程在读取过程中退出 | 不 oops(靠 `get_task_struct` 引用计数) |
| 7 | 目标是 `init`(pid 1) | 能读,不 oops |
| 8 | 非 root 用户访问 debugfs 接口 | `-EPERM` |
| 9 | 一个有 2000+ VMA 的进程 | 不超时、不爆栈 |
| 10 | `rmmod` 之后再 `insmod` | 无泄漏(`/proc/slabinfo` 对比) |

用例 4/5/6 是 A1 真正的价值所在。**它们是错误路径,而错误路径在内核里就是崩机
路径。** 用例 6 要特意构造:在 diff 脚本里改成读取时 `kill -9` 目标,循环 100 次。

用例 9 的构造程序 `tests/progs/many-vmas.c`:

```c
/* many-vmas.c: force a large VMA count by mapping pages with gaps, so the
 * kernel cannot merge them into one VMA. */
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#define N 2000

int main(void)
{
	int i;

	for (i = 0; i < N; i++) {
		/* PROT_NONE gap between each mapping prevents VMA merging. */
		if (mmap(NULL, 4096, i % 2 ? PROT_READ : PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
			return 1;
	}
	printf("pid=%d vmas>=%d\n", getpid(), N);
	fflush(stdout);
	for (;;)
		pause();
	return 0;
}
```

交替 `PROT_READ` / `PROT_READ|PROT_WRITE` 是为了**阻止内核合并 VMA** —— 相邻且
flags 相同的匿名 VMA 会被 `vma_merge()` 合成一个,那样就造不出 2000 个了。

### 5.3 加入 CI

`ci/zdtm-allowlist.txt` 此时还是空的(还没有 dump 能力)。A1 加的是
`tests/ci-smoke.sh` 里的一行:

```sh
sh tests/compare/diff-maps.sh || exit 1
```

---

## 6. 完成标准

- [ ] 10 个测试用例全部通过
- [ ] 在 `CONFIG_DEBUG_VM` + `PROVE_LOCKING` + `KASAN` 全开的内核上 dmesg 干净
- [ ] `diff-maps.sh` 进入 `tests/ci-smoke.sh`,CI 绿
- [ ] `criu_walk_vmas` / `criu_classify_vma` / `criu_get_task` 三个接口签名冻结
      (A3 起依赖它们,改签名要同步改下游)
- [ ] checkpatch `--strict` 无 error
- [ ] sparse 无 address-space warning
