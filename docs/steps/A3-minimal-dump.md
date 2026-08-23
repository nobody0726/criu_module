# A3 —— 极简进程完整 dump(里程碑)

**工期:** 2-3 周 · **前置:** A2 · **产出:** `criu restore` 能恢复本模块产出的镜像

> 相关原理:[04-image-format](../principles/04-image-format.md)、
> [03-memory-and-vma](../principles/03-memory-and-vma.md)、
> [05-registers-and-sigframe](../principles/05-registers-and-sigframe.md)

---

## 1. 设计思路

### 这一步是门禁

A3 是整个计划里第一个真正的里程碑,也是**门禁**:A3 不通过,A4-A8 全部无意义。
它要证明的命题只有一个:

> **本模块产出的镜像,真 `criu restore` 认得。**

一旦这个命题成立,后面每一步都有了免费的、极强的端到端验证器。这就是为什么要
花 2-3 周把它做通,而不是先铺开做多线程和 fd。

### 关键手法:oracle 反向使用

A3 一行 restore 代码都不写。流程是:

```
本模块 dump 极简进程 P  ──►  /tmp/imgs/  ──►  criu restore -D /tmp/imgs
                                                     │
                                              restore 成功
                                                     │
                                              ⇒ dump 是正确的
```

`criu restore` 是一个极其严格的验证器。它会校验镜像版本、字段完整性、
交叉引用一致性(比如 `pstree.img` 里提到的每个 pid 都得有对应的 `core-$pid.img`),
而且**它自己有大量断言**(前面看到的 `if (vpid(current) != pid) goto err` 就是
其中一个)。任何一处不对,它会报出明确的错误信息而不是静默错误。

用它当验证器,等于免费获得了一个比你自己能写出来的任何测试都严格的检查器。

### 「极简」要多简 —— 这个定义决定成败

**目标进程必须简到近乎人造。** 每多一个特性,镜像里就多一类文件,多一份出错可能。
A3 的目标:

| 维度 | A3 的要求 | 为什么 |
|---|---|---|
| 链接 | **静态链接** | 动态链接会带来 `ld.so`、多个 file-backed VMA、TLS |
| 线程 | **单线程** | 多线程要 `core-$tid.img`,是 A4 |
| fd | **只有 0/1/2**,且都重定向到普通文件 | tty 需要 `--shell-job` 和会话处理 |
| 信号 | **无 handler,无 pending** | `sigacts` 是 A6 |
| 定时器 | **无** | A6 |
| 共享内存 | **无** | A8 |
| 子进程 | **无** | A7 |
| socket | **无** | A5 |
| 当前目录 | `/` | 避免 mount namespace 复杂度 |

**这不是偷懒,是把变量控制到 1。** A3 失败时,你需要能确定失败原因在「dump 的基本
框架」而不是「某个特性没处理」。

### 镜像格式:不发明,照抄

原大纲 7.3 节提议「设计简单的二进制格式 / TLV / JSON」。**本计划否决这个方向。**

理由不是 protobuf 更好,而是:**只有二进制兼容 CRIU 格式,才能拿到 `criu restore`
和 ZDTM 当验证器。** 自定义格式意味着你必须同时写 dump 和 restore 才能验证任何
东西 —— 那就回到了原大纲那条九环长链。

代价是内核里要有 protobuf 编码。方案:用 `protobuf-c` 生成 C 代码,把生成的
`*.pb-c.c` 编进模块。`protobuf-c` 生成的代码只依赖 `malloc`/`free`/`memcpy`,
用宏替换成 `kmalloc`/`kfree` 即可。**不要手写 protobuf 编码** —— varint 和
字段顺序的坑足够耗掉一周。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 极简进程会产出哪些镜像

对 A3 定义的极简进程,`criu dump` 产出的镜像文件集大致是:

| 文件 | 内容 | A3 必须写 |
|---|---|---|
| `inventory.img` | 全局元信息:镜像版本、`root_ids` | 是 |
| `pstree.img` | 进程树平表 | 是 |
| `core-$pid.img` | 寄存器、信号掩码、rlimit、TLS | 是 |
| `mm-$pid.img` | VMA 列表 + `brk`/`start_code`/`arg_start` 等 | 是 |
| `pages-1.img` | 页内容(裸字节流) | 是 |
| `pagemap-$pid.img` | 页内容的索引:哪个虚拟地址对应 `pages-1.img` 里哪个偏移 | 是 |
| `files.img` | 全局文件表 | 是 |
| `fdinfo-$id.img` | 每任务的 fd → 文件表条目映射 | 是 |
| `reg-files.img` | 常规文件的路径/flags/pos | 是 |
| `ids-$pid.img` | 各 namespace / cgroup 的 id 引用 | 是 |
| `fs-$pid.img` | cwd 和 root | 是 |
| `creds-$pid.img` | uid/gid/capabilities | 是 |

**先跑一遍真 criu,看它到底产出什么,这是 A3 的第一天该做的事:**

```bash
./tests/progs/minimal &
PID=$!
criu dump -t $PID -D /tmp/ref-imgs -v4 --leave-running
ls -la /tmp/ref-imgs/
for f in /tmp/ref-imgs/*.img; do
	echo "=== $f ==="
	crit decode -i "$f" --pretty | head -40
done > /tmp/ref-decoded.txt
```

`/tmp/ref-decoded.txt` 就是 A3 的**规格说明书**。你要产出的每个字段,它里面都有
一个正确答案。

### 2.2 `pagemap` 与 `pages` 的分工

这是 A3 最容易搞错的一处。CRIU 把内存拆成两个文件:

- `pages-1.img` —— **纯裸字节**,没有任何头部、没有分隔符,就是一页接一页
- `pagemap-$pid.img` —— 索引,每条记录说「虚拟地址 X 起的 N 个页,在 pages 文件里
  从第 M 页开始」

参照 `criu/images/pagemap.proto`。这样设计的原因是 `pages` 文件可以用
`splice()` 零拷贝写入,不能掺任何元数据。

**注意 `pagemap` 里的偏移单位是「页」不是「字节」**,而且第一条记录是特殊的
`pagemap_head`(记 `pages_id`)。这两点各值一个 bug。

### 2.3 `should_dump_page` —— 别把所有页都存

`criu/criu/mem.c:105-175`。A3 必须实现它的核心跳过逻辑,否则:

- 会 dump 大量全零页(镜像巨大)
- 会 dump guard page(`PROT_NONE`),restore 时写入会失败
- 会 dump 未 COW 的 file-private 页(浪费,且和 `reg-files` 重复)

以及 `criu/criu/mem.c:451`:

```c
	if (!vma_area_is_private(vma, kdat.task_size) &&
	    !vma_area_is(vma, VMA_ANON_SHARED))
```

即:**只有 private VMA 和 anon-shared VMA 的内容需要进 pages 文件。**
`FILE_SHARED` 的内容一个字节都不存 —— 它的所有者是 inode。

`vma_entry_is_private` 的定义在 `criu/criu/include/vma.h:104`:

```c
static inline bool vma_entry_is_private(VmaEntry *entry, unsigned long task_size)
{
	return (vma_entry_is(entry, VMA_AREA_REGULAR) &&
		(vma_entry_is(entry, VMA_ANON_PRIVATE) ||
		 vma_entry_is(entry, VMA_FILE_PRIVATE)) &&
		(entry->end <= task_size)) ||
	       vma_entry_is(entry, VMA_AREA_SHSTK) ||
	       vma_entry_is(entry, VMA_AREA_AIORING);
}
```

### 2.4 vDSO 必须整段 dump

`criu/criu/mem.c` 的 `should_dump_entire_vma()`:vDSO 和 AIORING 区域**总是**
完整 dump,不做任何跳过优化。

vDSO 是内核映射进每个进程的一小段代码。它的地址和内容跨内核版本会变,所以 CRIU
把它整段存下来,restore 时做符号级的重定位(`criu/criu/pie/util-vdso.c`)。
A3 只需要**正确标记它是 vDSO**(`VMA_AREA_VDSO`),重定位由 `criu restore` 完成。

标记方法:比较 `vma->vm_start` 与 `mm->context.vdso`,或检查
`vma->vm_ops == &special_mapping_vmops` 且名字是 `[vdso]`。

**忘记标记 vDSO 是 A3 最常见的失败原因** —— restore 会在 vDSO 重定位阶段报错,
错误信息还相当难懂。

### 2.5 寄存器:`core.proto` 的 `thread_info_x86`

`criu/images/core-x86.proto` 定义 `user_x86_regs_entry`。字段名和
`struct pt_regs` 几乎一一对应,但**顺序不同**,而且 CRIU 用的是
`user_regs_struct`(ptrace ABI)的语义,不是 `pt_regs`(内核内部)的语义。

在 x86_64 上二者布局其实很接近,但有陷阱:

- `orig_ax` 的语义:系统调用号,或 -1 表示不在 syscall 中
- 段寄存器 `cs`/`ss`/`ds`/`es`/`fs`/`gs` 和 `fs_base`/`gs_base` 要分开处理
- FPU/SSE 状态在 `user_fpregs_entry` 里,单独一块

A3 的极简进程如果在 `pause()` 里被冻结,它是**在系统调用中**被停的,`orig_ax`
会是 `__NR_pause`。restore 时 CRIU 会正确处理系统调用重启。**这是对的,不要试图
把它归零。**

### 2.6 `start_time` 不用管

`criu/images/core.proto:64`:

```protobuf
	// Reserved for container relative start time
	//optional uint64		start_time	= 19;
```

**字段是注释掉的。** CRIU 不 dump 也不 restore 进程启动时间 —— 内核没有写接口。
A3 不用管它。这个洞是 X1 步骤要补的。

**要抄的:** 全部镜像格式、`should_dump_page` 逻辑、vDSO 标记、pagemap/pages 分工。
**不抄的:** parasite 注入(我们直接读内核);`/proc` 文本解析。

---

## 3. 文件结构

**Create:**
- `kernel_module/serialize/pb.c` —— protobuf-c 的内核适配层(kmalloc 包装)
- `kernel_module/serialize/img_file.c` —— 镜像文件写入(`filp_open`/`kernel_write`)
- `kernel_module/checkpoint/dump_core.c` —— `core-$pid.img`
- `kernel_module/checkpoint/dump_mm.c` —— `mm-$pid.img` + `pagemap` + `pages`
- `kernel_module/checkpoint/dump_files.c` —— `files.img`/`fdinfo`/`reg-files.img`(只处理常规文件)
- `kernel_module/checkpoint/dump_misc.c` —— `inventory`/`pstree`/`ids`/`fs`/`creds`
- `kernel_module/checkpoint/dump.c` —— 编排:freeze → collect → write → thaw
- `kernel_module/images/` —— 从 `criu/images/*.proto` 生成的 `*.pb-c.[ch]`
- `tests/progs/minimal.c`
- `tests/cross-restore.sh`
- `userspace/criu-shim/criu-shim.c`

**Modify:**
- `kernel_module/Makefile` —— 加 protobuf-c 生成规则
- `ci/zdtm-allowlist.txt` —— 从空变成有内容

**Interfaces produced:**

```c
/* One dump session. Everything the dump needs, so no globals. */
struct criu_dump_ctx {
	struct criu_freeze_ctx	*freeze;
	struct file		*img_dir;	/* O_PATH handle to image dir */
	pid_t			root_vpid;
	u32			pages_id;	/* pages-N.img id, 1 for A3 */
	loff_t			pages_off;	/* running byte offset */
	u64			nr_pages_written;
};

/* Top-level entry. Freezes, dumps, thaws. Never leaves tasks frozen on
 * error -- the error path always thaws. */
int criu_do_dump(pid_t vpid, const char *img_dir_path);

/* Image writing. criu_img_open returns a *file, not an fd: a module has no
 * fd table of its own to install into. */
struct file *criu_img_open(struct criu_dump_ctx *ctx, const char *name);
int criu_img_write_pb(struct file *f, const ProtobufCMessage *msg);
int criu_img_write_raw(struct file *f, const void *buf, size_t len,
		       loff_t *pos);
void criu_img_close(struct file *f);
```

`criu_img_open` 返回 `struct file *` 而不是 fd,是一个必须一开始就想清楚的设计点:
**内核模块没有自己的 fd 表。** `filp_open()` 给你 `struct file *`,配
`kernel_write()` 直接用。不要试图 `get_unused_fd_flags()` —— 那会往**当前进程**
(可能是 `insmod` 的那个 shell,也可能是某个 kworker)的 fd 表里塞东西。

---

## 4. 关键实现要点

### 4.1 镜像文件都有一个 magic 头

每个 `.img` 文件开头是 4 字节 magic + 4 字节的每条记录长度前缀。参照
`criu/criu/include/image.h` 里的 `IMG_COMMON_MAGIC` 和各类型的 magic 常量,以及
`criu/criu/image.c` 的 `do_open_image()`。

格式(小端):

```
[4 bytes] magic
然后重复:
  [4 bytes] size of the following protobuf message
  [size bytes] serialized protobuf
```

`pages-N.img` 是**例外** —— 它只有 magic,后面是裸页数据,没有长度前缀。

漏掉 magic 是 A3 第一天必然会撞的错,`criu restore` 会报
`Unknown magic 0x0 on ...`。这个错误信息很清楚,不用怕。

### 4.2 protobuf-c 在内核里

生成:

```bash
protoc-c --c_out=kernel_module/images -Icriu/images criu/images/core.proto
```

适配层 `pb.c` 需要提供 allocator:

```c
/* protobuf-c lets us supply an allocator; point it at the slab so we never
 * call libc malloc (which does not exist here). */
static void *criu_pb_alloc(void *data, size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void criu_pb_free(void *data, void *ptr)
{
	kfree(ptr);
}

ProtobufCAllocator criu_pb_allocator = {
	.alloc = criu_pb_alloc,
	.free = criu_pb_free,
	.allocator_data = NULL,
};
```

**编译 `*.pb-c.c` 时要注意**:生成的代码可能引用 `assert()`、`strlen()`、
浮点。前两个用内核等价物替换,浮点(`double` 字段)在我们用到的 proto 里没有 —— 
但要在 Makefile 里加 `-mno-sse` 之外的检查,因为内核态用 SSE 需要显式
`kernel_fpu_begin()`。**核实一遍生成的代码里没有浮点,是 A3 的第一天任务之一。**

### 4.3 写 pages 与 pagemap 的顺序

必须**同时**推进两个文件,而且 pagemap 的偏移是「页序号」:

```c
	/* pagemap records offsets in PAGES, not bytes, and the first record
	 * in the file is a pagemap_head carrying pages_id.
	 */
	pm_entry.vaddr = addr;
	pm_entry.nr_pages = run_len;
	/* implicit: these pages land at ctx->pages_off / PAGE_SIZE */

	ret = criu_img_write_raw(pages_file, page_buf,
				 run_len * PAGE_SIZE, &ctx->pages_off);
```

连续的可 dump 页要**合并成一条 pagemap 记录**(`nr_pages > 1`),否则一个 1GB 的
进程会产生 262144 条记录。真 criu 是这么做的,而且 `criu restore` 对记录数量没有
硬限制,但镜像会大得离谱。

### 4.4 读页内容:选 `access_process_vm` 还是 `get_user_pages_remote`

S0 已确认两者都可用。A3 的选型:

| | `access_process_vm` | `get_user_pages_remote` |
|---|---|---|
| 接口 | 给 buffer,内核帮你拷 | 给你 `struct page *` 数组 |
| 代码量 | 少 | 多(要自己 `kmap`) |
| 拷贝次数 | 1 次(到你的 buffer)+ 1 次(写文件) | 可做到 1 次 |
| 是否会触发缺页 | 会(可能分配新页!) | 可控(`FOLL_` flags) |

**A3 选 `access_process_vm`,理由是简单。** 但要注意一个陷阱:它会**触发缺页
处理**,也就是说读一个尚未分配的匿名页会导致内核**分配一个零页**。这会让你的
dump 产生原本不存在的页,并且改变目标进程的 RSS。

规避:先用 `/proc/PID/pagemap` 的等价内核路径判断页是否 present,只读 present 的
页。判断方法是走页表:

```c
	/* Walk the page tables without faulting anything in. If any level is
	 * absent, the page is not present and must not be dumped -- reading it
	 * would allocate a zero page that the process never had.
	 */
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return false;
	/* ... p4d, pud, pmd, pte ... */
	return pte_present(*pte);
```

这段是 A3 技术上最硬的一块,也是内核模块相对 CRIU 真正省事的地方 —— CRIU 要读
`/proc/PID/pagemap` 二进制接口再解析,我们直接走页表。

**性能优化留到以后。** A3 的目标是「对」,不是「快」。

### 4.5 错误路径必须 thaw

```c
int criu_do_dump(pid_t vpid, const char *img_dir_path)
{
	struct criu_dump_ctx ctx = {};
	int ret;

	ret = criu_freeze(vpid, false, &ctx.freeze);
	if (ret)
		return ret;

	ret = do_dump_all(&ctx);

	/* Unconditional: leaving tasks frozen on an error path strands them
	 * permanently -- only a reboot recovers.
	 */
	criu_thaw(ctx.freeze);
	return ret;
}
```

**任何 `return` 都不能跳过 `criu_thaw`。** 这是 A2 用例 12 的另一面。

---

## 5. 如何测试

### 5.1 极简目标程序

```c
/* minimal.c: the simplest process that is still interesting to checkpoint.
 * Build static, no threads, no signal handlers, no timers.
 *
 *   gcc -static -O0 -o minimal minimal.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* Heap and stack data with known contents, so a restored process can prove
 * its memory survived byte-for-byte. */
#define HEAP_SZ 8192
#define PATTERN 0x3C

int main(void)
{
	char *heap;
	char stack_buf[4096];
	unsigned long tick = 0;

	heap = malloc(HEAP_SZ);
	if (!heap)
		return 1;
	memset(heap, PATTERN, HEAP_SZ);
	memset(stack_buf, PATTERN ^ 0xFF, sizeof(stack_buf));

	printf("pid=%d heap=%p stack=%p\n", getpid(), heap, stack_buf);
	fflush(stdout);

	for (;;) {
		/* Verify our own memory every second. If a restore corrupts a
		 * byte, the process itself detects it and says so -- much
		 * easier to debug than an external comparison.
		 */
		if (heap[0] != PATTERN || heap[HEAP_SZ - 1] != PATTERN) {
			printf("HEAP CORRUPT at tick %lu\n", tick);
			fflush(stdout);
			return 2;
		}
		if ((unsigned char)stack_buf[0] != (unsigned char)(PATTERN ^ 0xFF)) {
			printf("STACK CORRUPT at tick %lu\n", tick);
			fflush(stdout);
			return 3;
		}
		printf("tick=%lu\n", tick++);
		fflush(stdout);
		sleep(1);
	}
	return 0;
}
```

**让进程自己检查自己的内存**,是这个测试程序最重要的设计。restore 之后如果有一个
字节错了,进程会自己喊出来 —— 比你在外面比对 hexdump 好用一百倍。

`tick` 计数器则证明**执行是从 checkpoint 点继续的,不是重新开始的**:restore 后
第一行输出应该是 `tick=N+1`,不是 `tick=0`。

### 5.2 交叉恢复测试

```sh
#!/bin/sh
# A3 acceptance: our dump must be restorable by the real criu. This is the
# gate for the whole A track.
set -e

IMGS=/tmp/a3-imgs
rm -rf $IMGS && mkdir -p $IMGS

./tests/progs/minimal > /tmp/a3.out 2>&1 &
sleep 3
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/a3.out)
LAST_TICK=$(grep -o 'tick=[0-9]*' /tmp/a3.out | tail -1 | cut -d= -f2)
echo "target pid=$PID last tick=$LAST_TICK"

# --- dump with OUR module ---
insmod kernel_module/criu_kernel.ko
echo "$PID $IMGS" > /sys/kernel/debug/criu/dump
rmmod criu_kernel

# --- structural check: crit must be able to decode everything ---
for f in $IMGS/*.img; do
	case "$(basename "$f")" in
	pages-*.img) continue ;;   # raw bytes, not protobuf
	esac
	crit decode -i "$f" > /dev/null || { echo "FAIL: crit cannot decode $f"; exit 1; }
done
echo "crit decode: OK"

# --- the real test: let the real criu restore it ---
kill -9 $PID 2>/dev/null || true
sleep 1

criu restore -D $IMGS -v4 --log-file /tmp/a3-restore.log \
	--restore-detached >> /tmp/a3.out 2>&1 || {
	echo "FAIL: criu restore rejected our images"
	tail -60 /tmp/a3-restore.log
	exit 1
}
sleep 3

# --- the restored process must be alive, same pid, and continuing ---
kill -0 $PID || { echo "FAIL: restored process not alive"; exit 1; }

NEW_TICK=$(grep -o 'tick=[0-9]*' /tmp/a3.out | tail -1 | cut -d= -f2)
[ "$NEW_TICK" -gt "$LAST_TICK" ] || {
	echo "FAIL: not continuing ($LAST_TICK -> $NEW_TICK)"; exit 1; }

# It must have RESUMED, not restarted from zero.
[ "$NEW_TICK" -ge "$LAST_TICK" ] || { echo "FAIL: restarted"; exit 1; }

# And it must not have detected its own memory being corrupted.
if grep -qE 'HEAP CORRUPT|STACK CORRUPT' /tmp/a3.out; then
	echo "FAIL: restored process reports memory corruption"
	grep -E 'CORRUPT' /tmp/a3.out
	exit 1
fi

kill -9 $PID 2>/dev/null || true
echo "A3 OK -- our dump is restorable by real criu"
```

### 5.3 逐字段对照(A3 的开发期主力手段)

交叉恢复是全或无的。开发过程中需要更细的反馈,所以还要一个字段级比对:

```sh
#!/bin/sh
# Development aid: diff our images against real criu's for the same process.
# Fields legitimately differ (timestamps, our own pid), so the allowlist below
# is part of the test, not a workaround.
set -e

./tests/progs/minimal > /tmp/cmp.out & sleep 2
PID=$(sed -n 's/^pid=\([0-9]*\).*/\1/p' /tmp/cmp.out)

criu dump -t $PID -D /tmp/ref -v4 --leave-running
insmod kernel_module/criu_kernel.ko
echo "$PID /tmp/ours" > /sys/kernel/debug/criu/dump
rmmod criu_kernel

for img in core-$PID mm-$PID pstree files reg-files creds-$PID fs-$PID; do
	f=$img.img
	[ -f /tmp/ref/$f ] || continue
	crit decode -i /tmp/ref/$f  --pretty > /tmp/ref-$img.json
	crit decode -i /tmp/ours/$f --pretty > /tmp/ours-$img.json
	echo "=== $f ==="
	# dump_uptime and criu_run_id are per-run by construction.
	diff -u /tmp/ref-$img.json /tmp/ours-$img.json \
		| grep -vE '"(dump_uptime|dump_criu_run_id)"' || true
done
```

**注意这个脚本刻意不 `exit 1`。** 它是开发期的观察工具,不是门禁。门禁是 5.2。
把它做成门禁会导致你去追一堆无害的字段差异(比如 CRIU 写了某个 optional 字段而
你没写,但 restore 并不需要它)。

### 5.4 必须覆盖的测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | `crit decode` 能解码所有非 pages 镜像 | 无报错 |
| 2 | `criu restore` 成功 | 退出码 0 |
| 3 | 恢复后 pid 相同 | `kill -0 $PID` 成功 |
| 4 | 恢复后从 checkpoint 点继续 | `tick` 递增且不归零 |
| 5 | 堆内存字节完整 | 无 `HEAP CORRUPT` |
| 6 | 栈内存字节完整 | 无 `STACK CORRUPT` |
| 7 | 三个 fd 可用 | 恢复后 `printf` 还能输出 |
| 8 | 全零页未被 dump | `pages-1.img` 大小 < RSS |
| 9 | guard page 未被 dump | 镜像里无 `PROT_NONE` 区域的页 |
| 10 | vDSO 被正确标记 | `crit decode mm-$PID.img` 里有 `VMA_AREA_VDSO` |
| 11 | dump 期间目标被 `kill -9` | 不 oops,返回错误,已 thaw |
| 12 | 镜像目录不可写 | 返回 `-EACCES`,已 thaw |
| 13 | 连续 dump 同一进程 10 次 | 10 次都能 restore;无内存泄漏 |
| 14 | dump 一个 200MB RSS 的进程 | 完成,镜像大小合理 |

用例 8 和 9 检验 `should_dump_page` 真的生效了。**没有它们,一个「把所有页都存
下来」的实现也能通过 restore 测试** —— 它只是慢和大,不是错。这类「能过但错」的
情况是测试设计里最需要主动防的。

用例 10 单独列出来,因为忘记标记 vDSO 是 A3 最常见的失败,而且它的症状(restore
在 vDSO 重定位阶段失败)不容易联想到原因。

### 5.5 criu-shim:接上 ZDTM

A3 完成后,写一个 shim 冒充 criu 二进制,让 ZDTM 能跑:

```c
/* criu-shim: pretend to be the criu binary so zdtm.py can drive our module.
 *
 *   dump    -> talk to /sys/kernel/debug/criu/dump
 *   restore -> exec the real criu (we have no restore side; that is the point)
 *   check   -> exec the real criu
 *
 * zdtm.py is invoked with --criu-bin pointing here (see
 * criu/test/zdtm.py:3083).
 */
```

行为:

| 子命令 | shim 做什么 |
|---|---|
| `dump` | 解析 `-t` / `-D`,写入 debugfs,把内核日志转成 criu 风格日志 |
| `restore` | **直接 exec 真 criu** |
| `check` / `pre-dump` / 其他 | 直接 exec 真 criu |

这样 ZDTM 的每个测试就变成了「用我们的 dump + 真 criu 的 restore」—— 正是我们
要的交叉验证,而且一次拿到 489 个测试程序。

然后建立 `ci/zdtm-allowlist.txt`:

```
# ZDTM tests that pass with the kernel-module dump path.
# This file IS the progress dashboard: line count = progress.
# Add a test only when it passes locally 3 runs in a row.
zdtm/static/env00
zdtm/static/pid00
zdtm/static/caps00
```

**这个文件的行数就是项目进度。** 而且它给出了 A4-A8 的排序依据:跑一遍全套,
统计失败原因,**哪一类原因阻塞了最多测试就先做那一类**。这让「下一步做什么」
从一个要猜的问题变成一个可查的问题:

```sh
# The sorting dashboard: which missing feature blocks the most tests?
sudo ./criu/test/zdtm.py run -a --criu-bin $PWD/userspace/criu-shim/criu-shim \
	2>&1 | tee /tmp/zdtm-all.log
grep -oE 'Unsupported|not supported|unhandled [a-z]+' /tmp/zdtm-all.log \
	| sort | uniq -c | sort -rn
```

---

## 6. 完成标准

- [ ] 14 个用例全部通过
- [ ] `cross-restore.sh` 进 CI,绿
- [ ] `ci/zdtm-allowlist.txt` 至少 3 个测试
- [ ] 跑过一次全量 ZDTM,产出「失败原因统计表」,写进本文件的附录
- [ ] 用统计表确定 A4/A5/A6/A7 的实施顺序,更新 `03-Iteration-Plan.md`
- [ ] dmesg 干净;`rmmod` 后无 slab 泄漏
- [ ] 镜像格式没有任何自创字段(与 `criu/images/*.proto` 严格一致)

## 7. 如果 restore 一直不通

按这个顺序缩小范围:

1. `criu restore -v4` 的日志里搜第一个 `Error` —— CRIU 的错误信息通常直接点名
   哪个镜像的哪个字段有问题
2. `crit decode` 对比同一个镜像的 ref 版和 ours 版,只看 restore 报错涉及的那个
3. 把镜像**逐个**替换成真 criu 的版本,二分定位是哪个文件的问题:
   ```sh
   cp /tmp/ref/core-$PID.img /tmp/ours/    # 换掉 core,看还报不报错
   ```
   **这个二分法是 A3 最有效的调试手段**,因为它把「12 个镜像里哪个错了」从
   猜变成了 log₂(12) ≈ 4 次实验。
4. 都不行时,用 `strace -f criu restore` 看它在哪个系统调用上失败
