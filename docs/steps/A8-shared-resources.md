# A8 —— 跨进程共享资源

**工期:** 2-3 周 · **前置:** A5、A7 · **产出:** 共享内存 + 跨进程共享 fd

> 相关原理:[07-fd-and-shared-objects](../principles/07-fd-and-shared-objects.md)、
> [03-memory-and-vma](../principles/03-memory-and-vma.md)

---

## 1. 设计思路

### A8 是 A5 那个模式的第二次应用

A5 已经把去重的核心机制建好了:`criu_objmap_get(map, obj, &is_new)`。A8 做的事情
在结构上完全一样,只是被去重的对象换了:

| 步骤 | 被去重的内核对象 | 「同一个」的判据 |
|---|---|---|
| A5 | `struct file *` | 指针相等 |
| A8 | `struct files_struct *`(整张 fd 表) | 指针相等 |
| A8 | `struct mm_struct *`(整个地址空间) | 指针相等 |
| A8 | shmem inode / SysV shm 段 | inode 指针相等 |
| A8 | `struct fs_struct *`(cwd/root) | 指针相等 |

**如果 A5 把 `criu_objmap` 做对了,A8 的代码量比看起来小很多。** 这是 A5 那个
`is_new` 出参设计的回报兑现的地方。

### 但有一个新问题:作用域从「一个进程」变成「一个进程集合」

A5 里去重只需要在单个进程的 fd 表内进行。A8 里,两个不同进程的 fd 3 可能指向
同一个 `struct file` —— 这在 fork 之后很常见。

所以 `criu_objmap` 的生命周期必须是**整个 dump 会话**,而不是每进程一份。这一点
在 A5 里就该做对(`criu_dump_ctx` 持有它,而不是每进程新建一个),A8 只是让它
真正被用起来。

**A8 的第一天任务:检查 A5 的 objmap 是挂在 `criu_dump_ctx` 上而不是每进程一个。**

### 四类共享资源,难度差别很大

| 资源 | 难度 | 为什么 |
|---|---|---|
| `CLONE_FILES` 共享 fd 表 | 低 | 指针比较,`fdinfo` 指向同一个 id |
| `CLONE_FS` 共享 cwd/root | 低 | 同上 |
| SysV shm / POSIX shm | 中 | 有 inode 可作锚点,内容存一份 |
| 匿名 `MAP_SHARED`(无文件名的共享内存) | **高** | 见 1.4 |

### 1.4 匿名 MAP_SHARED 的难点

`mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0)`
之后 fork,父子共享这块内存。

内核里它其实**不是匿名的** —— 内核为它偷偷创建了一个 shmem(tmpfs)inode。
所以 `vma->vm_file` 非空。这就是 A1 里那句警告的具体后果:

> `VMA_ANON_SHARED` **也有 `vm_file`**,所以不能靠 `vm_file == NULL` 判断匿名。

判据必须是:

```c
	/* An anonymous MAP_SHARED region is backed by an internal shmem inode,
	 * so vm_file is NOT NULL. Use the shmem test, not a vm_file test.
	 */
	if (vma->vm_file && shmem_file(vma->vm_file) &&
	    (vma->vm_flags & VM_SHARED))
		return CRIU_VMA_ANON_SHARED;
```

难点在于:这个 inode 在文件系统里**没有路径**(它在内部 tmpfs 里,不在任何挂载点
下)。restore 时没法 `open()` 它。CRIU 的解法是用一个「锚点」文件重建它 —— 见 2.3。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 fd 表共享:`fdinfo` 的 id 就是答案

`pstree.img` 里没有「谁和谁共享 fd 表」的字段。取而代之的是:
**共享 fd 表的两个进程,`ids-$pid.img` 里的 `files_id` 相同,于是它们读同一个
`fdinfo-$id.img`。**

参照 `criu/images/ids.proto`:

```protobuf
message task_kobj_ids_entry {
	required uint32			vm_id		= 1;
	required uint32			files_id	= 2;
	required uint32			fs_id		= 3;
	required uint32			sighand_id	= 4;
	...
}
```

**这五个 id 就是「五种 CLONE_* 共享」的镜像表示。** 两个 task 的 `files_id` 相等
⇔ 它们 `CLONE_FILES` 共享。restore 时,第二个 task 用 `CLONE_FILES` clone 出来,
而不是重新 open 一遍所有 fd。

这个设计非常干净,直接照抄。A8 的工作就是把这五个 id 用 `criu_objmap` 正确分配:

```c
	ids.vm_id      = criu_objmap_get(ctx->objmap, task->mm, &is_new);
	ids.files_id   = criu_objmap_get(ctx->objmap, task->files, &is_new);
	ids.fs_id      = criu_objmap_get(ctx->objmap, task->fs, &is_new);
	ids.sighand_id = criu_objmap_get(ctx->objmap, task->sighand, &is_new);
```

**注意 A3 已经写过 `ids-$pid.img` 了**,但当时是单进程,所有 id 随便给个常数也能过。
A8 要把它改成真正的去重分配。这是 A3「简到近乎人造」这个选择的一处欠账,现在偿还。

### 2.2 SysV / POSIX 共享内存

参照:
- `criu/images/shmem.proto` —— 共享内存段的描述
- `criu/images/ipc-shm.proto` —— SysV IPC 的 key/权限
- `criu/criu/shmem.c` —— 主逻辑

CRIU 的做法:每个共享内存段分配一个 `shmid`(镜像层面的 id,不是 SysV 的 shmid),
**内容只存一份**在 pages 文件里。所有映射了它的进程的 `mm-$pid.img` 里,对应的
VMA 用 `shmid` 引用它。

**「内容只存一份」是这一步的正确性核心。** 存多份的症状不是空间浪费,而是:
restore 时后写入的那份覆盖先写入的那份,如果两份在 dump 期间有差异(不该有,
因为冻结了),就会静默丢数据。更糟的是,restore 后两个进程可能映射到**两块不同的
内存**,共享关系断掉 —— 一个进程写,另一个读不到。

这个错误的隐蔽性和 A5 的 pipe 配对错误是同一类:**单向使用的测试查不出来。**

### 2.3 匿名共享内存的锚点

对没有路径的 shmem inode,CRIU 的做法(`criu/criu/shmem.c`)是:restore 时
在一个已知位置创建一个新的 shmem 对象(`memfd_create` 或 tmpfs 里的临时文件),
把内容填进去,让第一个进程 `mmap` 它,其余进程通过继承或再次 `mmap` 同一个对象
拿到同一块内存,最后 `unlink` 掉锚点文件。

**dump 侧要做的只有两件事:**
1. 给这个 shmem inode 分配一个 `shmid`(用 `criu_objmap_get(ctx->objmap, inode, &is_new)`)
2. 把它的内容存进 pages 文件一次

锚点的创建是 restore 侧的事(B 轨或真 criu)。**这是 A8 的边界:dump 侧只需要
「同一个对象给同一个 id,内容存一份」。**

### 2.4 一个反直觉的点:file-backed MAP_SHARED 不存内容

A3 已经建立了这条规则(`mem.c:451`),A8 要确认它在共享场景下仍然生效:

| VMA 类型 | 内容存进 pages? | 谁负责内容 |
|---|---|---|
| `VMA_ANON_PRIVATE` | 是 | pages 文件 |
| `VMA_FILE_PRIVATE` | 只存已 COW 的页 | pages 文件 + 原文件 |
| `VMA_ANON_SHARED` | **是**(存一份) | pages 文件 |
| `VMA_FILE_SHARED` | **否** | 文件本身(inode) |

`VMA_FILE_SHARED` 不存内容,因为那些脏页会被内核回写到文件里,文件就是权威副本。
**这也意味着一个已知的语义缺口:** 如果 dump 之后有别人改了那个文件,restore
后的进程看到的是改过的内容。CRIU 也有这个缺口,它不是 bug 而是设计。写进限制列表。

**要抄的:** 五个 `*_id` 的分配方式、shmem 内容存一份、锚点机制的 dump 侧部分。
**不抄的:** 锚点的创建(restore 侧)。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/dump_shmem.c` —— shmem / SysV shm
- `kernel_module/checkpoint/dump_ids.c` —— 五个 `*_id` 的真实分配
- `tests/progs/shm-anon.c` / `shm-sysv.c` / `shared-fdt.c`

**Modify:**
- `kernel_module/checkpoint/dump_misc.c` —— `ids-$pid.img` 从常数改为真实去重
- `kernel_module/checkpoint/dump_mm.c` —— VMA 的 `shmid` 字段
- `kernel_module/checkpoint/dump_fdtable.c` —— objmap 作用域扩到整个会话

**Interfaces produced:**

```c
/* Shared-object ids for one task. Two tasks that share a kernel object get
 * the same id here, which is exactly how the image format expresses
 * CLONE_FILES / CLONE_VM / CLONE_FS / CLONE_SIGHAND sharing. */
struct criu_task_ids {
	u32	vm_id;
	u32	files_id;
	u32	fs_id;
	u32	sighand_id;
	u32	ipc_ns_id;
	u32	mnt_ns_id;
};

int criu_alloc_task_ids(struct criu_dump_ctx *ctx, struct task_struct *task,
			struct criu_task_ids *out);

/* Register a shared-memory object and get its image-level id. Content is
 * written to the pages file only on the first call for a given inode, which
 * is what keeps exactly one copy in the image. */
int criu_shmem_register(struct criu_dump_ctx *ctx, struct inode *inode,
			unsigned long size, u32 *shmid_out, bool *is_new_out);

/* Write the content of a registered shmem object. Call only when
 * criu_shmem_register reported is_new. */
int criu_shmem_dump_content(struct criu_dump_ctx *ctx, struct inode *inode,
			    u32 shmid, unsigned long size);
```

`criu_shmem_register` 和 `criu_shmem_dump_content` **分成两个函数**,是刻意的:
它让「注册」和「写内容」在代码里明显分开,`is_new` 门控只在一处。如果合成一个
函数,「只写一次」这个约束就藏在函数内部,读代码的人无法一眼确认。

---

## 4. 关键实现要点

### 4.1 遍历 SysV 共享内存

SysV shm 段挂在 `ipc_namespace` 的 `ids[IPC_SHM_IDS]` 上,是一个 IDR。

```c
	/* SysV shm segments live in the IPC namespace, not in the task. Walk the
	 * IDR and keep only segments actually attached by a task in our set --
	 * dumping the whole namespace would capture other users' segments.
	 */
	struct ipc_ids *ids = &task->nsproxy->ipc_ns->ids[IPC_SHM_IDS];
```

**只 dump 被我们的进程集合 attach 的段。** 整个 namespace 里可能有别人的段,
把它们一起 dump 进去会让镜像包含无关数据,restore 时还会试图重建它们。

判断「被 attach」的方法:从 VMA 侧反向找 —— 遍历每个进程的 VMA,凡是
`vma->vm_file` 的 `f_op` 是 shm 的,就把对应的段登记进来。**从 VMA 走比从
IPC namespace 走更可靠**,因为它天然只覆盖真正被用到的段。

`ipc_ns` 相关符号在 5.10 是否可访问要在 S0 验证。若不可访问,退路:只支持
POSIX shm(`shm_open` + `mmap`)和匿名 `MAP_SHARED`,SysV shm 明确 `-EOPNOTSUPP`。
**这是可接受的退路** —— SysV shm 在现代应用里已经不常见。

### 4.2 读 shmem 内容

shmem 页在 page cache 里,直接从 `inode->i_mapping` 读:

```c
	/* shmem content lives in the page cache. Reading via the mapping avoids
	 * going through any one process's page tables, which matters because
	 * different processes may have mapped different subsets of the object.
	 */
	for (index = 0; index < nr_pages; index++) {
		page = find_get_page(inode->i_mapping, index);
		if (!page)
			continue;	/* hole: never written, skip like a zero page */
		/* kmap, copy out, kunmap, put_page */
	}
```

**从 mapping 读而不是从某个进程的页表读**,是这里的关键决定。原因:不同进程可能
只映射了这个对象的一部分,或者映射在不同的虚拟地址上。对象的内容属于对象,不属于
任何一个映射它的进程。

`find_get_page` 返回 NULL 表示这一页从未被写过(shmem 的洞)。跳过它,和 A3 里
跳过零页是同一个道理。

### 4.3 `vm_id` 与 `CLONE_VM` 但非 `CLONE_THREAD`

罕见但合法:`clone(CLONE_VM)` 不带 `CLONE_THREAD`,得到两个**共享地址空间的独立
进程**(不是线程)。它们的 `mm` 指针相同,但 `tgid` 不同。

`vm_id` 相同就正确表达了这个关系。但要注意:

```c
	/* Two tasks sharing an mm without CLONE_THREAD are separate processes.
	 * Dump the mm once (keyed on the mm pointer), but each still needs its
	 * own core-$pid.img -- registers are never shared.
	 */
```

**`mm` 存一份,寄存器每个都要。** A4 已经建立了这个划分,这里只是它的另一个入口。

CRIU 对这种情况的支持是有限的(`fork_with_pid` 里有
`BUG_ON(ca.clone_flags & CLONE_VM)`,在 `criu/criu/cr-restore.c:1189`)。
**所以 dump 侧检测到 `CLONE_VM` 且非 `CLONE_THREAD` 时,应该明确报 `-EOPNOTSUPP`**
—— 产出一份真 criu 会 `BUG_ON` 的镜像毫无意义。

### 4.4 引用计数

A8 引入了一个新风险:objmap 里存的指针必须在整个 dump 期间有效。A5 里对
`struct file` 是 `get_file()` 保证的。A8 里:

| 对象 | 取引用 | 放引用 |
|---|---|---|
| `mm_struct` | `mmget()` / `get_task_mm()` | `mmput()` |
| `files_struct` | `atomic_inc(&files->count)` | `put_files_struct()` |
| `fs_struct` | `fs->users++` 需持 `fs->lock` | `free_fs_struct()` |
| `sighand_struct` | `refcount_inc(&sighand->count)` | `__cleanup_sighand()` |
| shmem `inode` | `ihold()` | `iput()` |

**每一种的 put 函数是否导出要在 S0 一并验证。** 取不到 put 函数比取不到 get
函数更糟 —— 那会造成永久的引用泄漏,对象再也不会被释放。

对策:如果某个 put 不可用,就**不要持引用**,而是依赖 A2 的冻结保证。冻结期间
任务不运行,不会 exit,所以这些对象不会被释放。**这个依赖必须写在注释里**,因为
它把 A8 的正确性绑在 A2 的保证上。

```c
	/* We deliberately do NOT take a reference on fs_struct: free_fs_struct()
	 * is not exported, so a leaked reference would be permanent. Safety comes
	 * from A2 instead -- the tasks are frozen for the whole dump, so they
	 * cannot exit and drop the last reference. If the freeze guarantee ever
	 * weakens, this becomes a use-after-free.
	 */
```

---

## 5. 如何测试

### 5.1 共享内存测试程序

```c
/* shm-anon.c: parent and child share an anonymous MAP_SHARED region and take
 * turns writing to it. If the restore breaks the sharing, the two sides stop
 * seeing each other's writes and both report it.
 *
 *   gcc -static -O0 -o shm-anon shm-anon.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define SHM_SZ 65536

struct shared {
	unsigned long parent_seq;
	unsigned long child_seq;
	char pattern[4096];
};

int main(void)
{
	struct shared *sh;
	pid_t child;

	sh = mmap(NULL, SHM_SZ, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return 1;
	memset(sh, 0, SHM_SZ);
	memset(sh->pattern, 0x5A, sizeof(sh->pattern));

	child = fork();
	if (child < 0)
		return 1;

	if (child == 0) {
		unsigned long last = 0;

		printf("child pid=%d shm=%p\n", getpid(), (void *)sh);
		fflush(stdout);
		for (;;) {
			/* The child must see the parent's increments. If the
			 * restore split the region into two copies, parent_seq
			 * stops moving from here. */
			sh->child_seq = sh->parent_seq;
			if (sh->pattern[0] != 0x5A) {
				printf("child: SHM PATTERN CORRUPT\n");
				fflush(stdout);
				return 2;
			}
			if (sh->parent_seq == last && last > 3) {
				printf("child: SHARING BROKEN at %lu\n", last);
				fflush(stdout);
				return 3;
			}
			last = sh->parent_seq;
			printf("child: saw parent_seq=%lu\n", last);
			fflush(stdout);
			sleep(1);
		}
	}

	printf("parent pid=%d shm=%p\n", getpid(), (void *)sh);
	fflush(stdout);
	for (;;) {
		sh->parent_seq++;
		/* The parent must see the child's echo. */
		printf("parent: seq=%lu child_echo=%lu\n",
		       sh->parent_seq, sh->child_seq);
		if (sh->parent_seq > 4 &&
		    sh->child_seq + 3 < sh->parent_seq) {
			printf("parent: SHARING BROKEN\n");
			fflush(stdout);
			return 3;
		}
		fflush(stdout);
		sleep(1);
	}
	return 0;
}
```

**双向检验是这个测试的要点。** 父检查子的回声,子检查父的递增。一个「把共享内存
当成两块私有内存各存一份」的实现,会让两边都停在原地 —— 但只有双向检查才能保证
无论哪一边先卡住都被抓到。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 匿名 `MAP_SHARED` 父子共享 | 恢复后双向可见,无 `SHARING BROKEN` |
| 2 | 共享内存内容完整 | 无 `SHM PATTERN CORRUPT` |
| 3 | 共享内存**只在镜像里存一份** | pages 文件大小 ≈ 单份,不是双份 |
| 4 | 3 个进程映射同一块 | 三方互相可见 |
| 5 | 同一块内存在不同进程映射到**不同虚拟地址** | 各自的地址都恢复正确 |
| 6 | 部分映射(进程 A 映射前半,B 映射后半) | 各自看到正确的部分 |
| 7 | POSIX shm(`shm_open` + `mmap`) | 恢复后共享正常 |
| 8 | SysV shm | 恢复正常,或明确 `-EOPNOTSUPP` |
| 9 | `CLONE_FILES` 共享 fd 表 | `ids` 里 `files_id` 相同;恢复后一方 `dup` 另一方可见 |
| 10 | `CLONE_FS` 共享 cwd | 一方 `chdir` 另一方可见 |
| 11 | `CLONE_VM` 非 `CLONE_THREAD` | 明确 `-EOPNOTSUPP`(真 criu 会 `BUG_ON`) |
| 12 | `VMA_FILE_SHARED` 的内容**未**进 pages | pages 文件不含该区域 |
| 13 | 共享内存有洞(未写过的页) | 洞未被 dump,恢复后读到零 |
| 14 | 100MB 共享内存 | 完成,pages 文件 ≈ 100MB 不是 200MB |
| 15 | 同一个 `struct file` 被两个进程的不同 fd 号持有 | 同一个 file id,fd 号各自正确 |

用例 3 和 14 是本步骤**最重要的两个用例**,因为「存两份」的实现能通过所有功能
用例 —— 恢复后共享关系甚至可能碰巧是对的(如果 restore 端按 id 去重,而你给了
不同的 id,它就会建两块内存)。**必须直接测镜像大小。**

用例 3 的判定写法:

```sh
# One copy, not N copies: compare the pages file against the shared region size.
# 65536 bytes of shared memory mapped by 2 processes must add ~64K to the image,
# not ~128K.
BASE=$(stat -c %s /tmp/imgs-noshm/pages-1.img)
WITH=$(stat -c %s /tmp/imgs-shm/pages-1.img)
DELTA=$(( WITH - BASE ))
# Allow generous slack for the extra process's own pages, but 2x the region
# size must be out of range.
[ "$DELTA" -lt 98304 ] || { echo "FAIL: shared memory stored more than once"; exit 1; }
```

用例 5 值得单独说:同一个对象在不同进程映射到不同虚拟地址,是完全合法的
(`mmap` 的返回地址由内核选)。镜像里 VMA 的地址是每进程的,`shmid` 是共享的。
**把这两者搞混的实现会让所有进程的映射地址都变成第一个进程的**,症状是第二个
进程恢复后拿着一个野指针。

### 5.3 ZDTM 增量

```
zdtm/static/shmem
zdtm/static/shm
zdtm/static/mmap_anon_shared00
zdtm/static/mmap_anon_shared01
zdtm/static/fdt_shared
zdtm/static/cow01
zdtm/static/ipc_namespace
```

`zdtm/static/cow01` 特别有价值:它专门测 COW 语义,能抓住「把共享和私有搞混」的
实现。

---

## 6. 完成标准

- [ ] 15 个用例通过,含 3、5、14 三个「存一份」相关用例
- [ ] `criu_objmap` 的作用域确认是整个 dump 会话(代码审查)
- [ ] `ids-$pid.img` 的五个 id 是真实去重分配的,不是常数
- [ ] 每一处「不持引用、依赖冻结」的地方都有注释说明
- [ ] A3-A7 测试全部仍通过
- [ ] allowlist 增加至少 5 个测试
- [ ] 限制列表更新:`VMA_FILE_SHARED` 的语义缺口、`CLONE_VM` 不支持、
      可能的 SysV shm 不支持
