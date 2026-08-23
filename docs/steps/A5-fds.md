# A5 —— 文件描述符

**工期:** 2-3 周 · **前置:** A3 · **产出:** `files.img` / `fdinfo-*.img`,pipe / 常规文件 / unix socket

> 相关原理:[07-fd-and-shared-objects](../principles/07-fd-and-shared-objects.md)

---

## 1. 设计思路

### fd 是 B 类的典型样本

一个 fd 就是个 `int`,是 `task->files->fdt->fd[]` 数组的下标。这个数字本身毫无
信息 —— 真正的东西是数组那一格里的 `struct file *`,以及它背后的 inode、
socket、pipe buffer。

所以 dump 一个 fd 意味着**记录足够的信息,让 restore 时能重新 `open()` 出一个
等价的东西**。这就把 fd dump 分成了两个截然不同的子问题:

**子问题 1:这个 fd 指向什么?** 要记录的是「配方」——路径、flags、文件位置。
**子问题 2:两个 fd 是不是同一个东西?** 这就是共享/去重问题,是 A5 的真正难点。

### 为什么去重是难点

考虑这三种情况,它们在 `/proc/PID/fd/` 里长得几乎一样:

```
情况 A: fd 3 和 fd 4 各自 open("/tmp/x") 两次
        → 两个 struct file,各自独立的 f_pos。lseek(3) 不影响 fd 4
情况 B: fd 4 = dup(3)
        → 一个 struct file,共享 f_pos。lseek(3) 会改变 fd 4 的位置
情况 C: 父子进程 fork 后各持 fd 3
        → 一个 struct file,跨进程共享 f_pos
```

**这三种必须区分开,因为 restore 的动作完全不同**:A 是两次 `open`,B 是
`open` + `dup2`,C 是一个进程 `open` 然后通过继承或 `SCM_RIGHTS` 传给另一个。

内核里区分很容易:比较 `struct file *` 指针是否相等。**这是内核模块相对 CRIU 的
第三个真实优势** —— CRIU 必须用 `kcmp(pid1, pid2, KCMP_FILE, fd1, fd2)` 系统调用
来问内核「这两个 fd 是不是同一个 file」,一次一对,O(n²) 次系统调用。我们直接
比指针。

### 实现顺序:pipe → 常规文件 → unix socket

按「反馈速度」而不是「简单程度」排:

1. **pipe 先做。** 它逼你处理「内容也要 dump」(pipe buffer 里的字节)和「两端
   配对」(读端和写端是同一个 pipe),这两个是 A5 的核心难点的最小样本。
2. **常规文件第二。** 概念简单,但坑多(见 4.3),而且 zdtm 里覆盖它的测试最多。
3. **unix socket 最后。** 它有 pipe 的所有问题,加上地址绑定、连接状态、
   in-flight 的 `SCM_RIGHTS`(**socket 里可能正在传递另一个 fd**)。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 两级间接:`fdinfo` → `files`

CRIU 的 fd 镜像是两层的:

```
fdinfo-$id.img:  fd 3 → file id 0x1234
                 fd 4 → file id 0x1234   ← 同一个 id ⇒ 是 dup
files.img:       id 0x1234 → type=REG, 指向 reg-files.img 里的条目
reg-files.img:   path=/tmp/x, flags=O_RDWR, pos=42
```

**中间那层 file id 就是去重机制。** 两个 fd 指向同一个 `struct file` ⇒ 分配同一个
id。这个设计要照抄,因为它就是 `criu restore` 期望的格式。

参照文件:
- `criu/images/fdinfo.proto` —— `fdinfo_entry { id, fd, type, flags }`
- `criu/images/fown.proto` —— `F_SETOWN` 的信息
- `criu/images/regfile.proto` —— `reg_file_entry { id, flags, pos, fown, name, ... }`
- `criu/images/pipe.proto` + `criu/images/pipe-data.proto` —— pipe 分成「描述」和「内容」两个镜像
- `criu/criu/files.c` —— 主逻辑,`dump_one_file()` 按 `file->f_op` 分发

### 2.2 CRIU 怎么判断 fd 类型

`criu/criu/files.c` 里根据 `/proc/PID/fd/N` 的 `readlink` 结果和
`/proc/PID/fdinfo/N` 的内容分类。它必须靠字符串匹配(`pipe:[12345]`、
`socket:[67890]`、`anon_inode:[eventfd]`)。

**内核里做法完全不同,而且干净得多:** 比较 `file->f_op`。

```c
	/* Dispatch on f_op, the file's operation table -- this is what the
	 * kernel itself uses to tell file types apart. No string matching.
	 */
	if (file->f_op == &pipefifo_fops)
		return CRIU_FD_PIPE;
	if (S_ISSOCK(file_inode(file)->i_mode))
		return CRIU_FD_SOCKET;
	if (S_ISREG(file_inode(file)->i_mode))
		return CRIU_FD_REG;
```

坑:`pipefifo_fops` 是否导出要在 S0 验证。若未导出,退路是
`S_ISFIFO(file_inode(file)->i_mode)` —— 用 inode 模式判断,不需要任何导出符号。
**优先用 inode 模式判断,它更稳定。**

### 2.3 pipe 内容怎么读

CRIU 的做法很曲折(`criu/criu/pipes.c`):它 `tee()` 出 pipe 内容(非破坏性读),
再 `splice()` 到镜像文件。必须非破坏性,因为 dump 不能改变进程状态。

**内核里可以直接读 `pipe_inode_info->bufs[]` 环形缓冲区**,完全不动 pipe 状态。
这是第四个真实优势,而且这次省掉的不只是开销,是一整套 `tee`/`splice` 的复杂度。

```c
	struct pipe_inode_info *pipe = file->private_data;

	/* Read the ring buffer directly. Non-destructive by construction:
	 * we never touch pipe->head or pipe->tail.
	 */
	mutex_lock(&pipe->mutex);
	for (i = pipe->tail; i != pipe->head; i++) {
		struct pipe_buffer *buf = &pipe->bufs[i & (pipe->ring_size - 1)];
		/* buf->page, buf->offset, buf->len */
	}
	mutex_unlock(&pipe->mutex);
```

`i & (pipe->ring_size - 1)` 是环形索引,`ring_size` 是 2 的幂。5.10 用
`head`/`tail`;更老的内核用 `nrbufs`/`curbuf`。**版本差异要在注释里标明。**

### 2.4 「同一个 pipe 的两端」

`pipe(fd)` 给你两个 fd,两个不同的 `struct file`,但**同一个 inode**。所以配对
判据是 `file_inode(file)` 相等,不是 `file` 相等。

CRIU 的 `pipe_entry.pipe_id` 就是 inode 号。这个区分很容易写错成比较 `file`
指针,症状是恢复后 pipe 两端被拆成两个不相连的 pipe —— **写进去的数据读不出来**,
而且这个错误在单向使用 pipe 的测试里不会暴露。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/dump_fdtable.c` —— 遍历 fd 表 + id 分配 + 去重
- `kernel_module/checkpoint/dump_regfile.c`
- `kernel_module/checkpoint/dump_pipe.c`
- `kernel_module/checkpoint/dump_unixsk.c`
- `kernel_module/core/objmap.c` —— 内核对象指针 → 镜像 id 的映射表
- `tests/progs/fds-regfile.c` / `fds-pipe.c` / `fds-unixsk.c` / `fds-dup.c`

**Modify:**
- `kernel_module/checkpoint/dump_files.c` —— A3 里只处理了 0/1/2,现在通用化

**Interfaces produced:**

```c
/* Object id map: the deduplication core. Maps a kernel object pointer to the
 * image-level id, so two fds pointing at the same struct file get the same
 * id and the restore side knows to dup() rather than open() twice.
 *
 * Keyed on pointer identity. Objects are pinned (we hold a reference) for the
 * lifetime of the dump, so pointers cannot be recycled underneath us.
 */
struct criu_objmap;

struct criu_objmap *criu_objmap_new(void);
void criu_objmap_free(struct criu_objmap *map);

/* Returns an existing id for obj, or allocates a fresh one.
 * *is_new tells the caller whether it must now emit the object's own image
 * entry -- this is how we write each shared object exactly once. */
u32 criu_objmap_get(struct criu_objmap *map, const void *obj, bool *is_new);

/* fd table walk. Callback runs WITHOUT files_struct lock held: we snapshot
 * the table first (pinning each struct file) and then iterate. */
typedef int (*criu_fd_fn)(unsigned int fd, struct file *file, void *arg);
int criu_walk_fds(struct task_struct *task, criu_fd_fn fn, void *arg);
```

`criu_objmap_get` 的 `is_new` 出参是整个 A5 的枢纽:**它把「去重」和「每个对象
只写一次」统一成了一个操作。** 调用者的模式永远是:

```c
	id = criu_objmap_get(map, file, &is_new);
	/* fdinfo always references the id ... */
	write_fdinfo(fd, id);
	/* ... but the object's own entry is written only once. */
	if (is_new)
		write_file_entry(id, file);
```

这个模式 A8 会原样复用到共享内存上。**A5 把它做对,A8 就基本免费。**

---

## 4. 关键实现要点

### 4.1 遍历 fd 表的锁

```c
	struct files_struct *files = get_files_struct(task);
	struct fdtable *fdt;

	if (!files)
		return -ESRCH;

	rcu_read_lock();
	fdt = files_fdtable(files);
	for (i = 0; i < fdt->max_fds; i++) {
		struct file *f = rcu_dereference(fdt->fd[i]);
		if (!f)
			continue;
		if (!get_file_rcu(f))	/* may fail if racing with close() */
			continue;
		/* pin it into our snapshot array */
	}
	rcu_read_unlock();
```

`get_files_struct` 在 5.10 里**可能未导出** —— S0 要验证。退路:任务已冻结,
直接 `task->files` 加 `atomic_inc(&files->count)`。因为 A2 保证了目标不在运行,
竞态窗口实际不存在,但**这个假设必须写在注释里**。

`get_file_rcu` 可能失败(引用计数已经归零,正在被释放)。**这不是错误,要 continue。**

### 4.2 常规文件:记什么

| 字段 | 从哪来 | 陷阱 |
|---|---|---|
| path | `d_path(&file->f_path, ...)` | 见 4.3 |
| flags | `file->f_flags` | 要去掉 `O_CREAT`/`O_EXCL`/`O_TRUNC` —— restore 时重开不能再创建/截断 |
| pos | `file->f_pos` | 直接读,任务已冻结 |
| mode | `file->f_mode` | `FMODE_READ`/`FMODE_WRITE` |
| `st_dev`/`st_ino` | `file_inode(file)` | restore 时用来**校验**打开的是同一个文件 |
| fown | `file->f_owner` | `F_SETOWN` 设的信号接收者 |

`O_TRUNC` 那条:如果原来是 `open(path, O_RDWR|O_TRUNC)`,restore 时照原样打开会
**清空文件**。CRIU 在 `reg_file_entry` 里存的是清理过的 flags。**这是一个会
静默毁数据的坑。**

### 4.3 路径的四个麻烦

| 麻烦 | 表现 | 处理 |
|---|---|---|
| 文件已被删除 | `d_path` 返回 `/tmp/x (deleted)` | CRIU 把内容存进 `ghost-file` 镜像,restore 时重建。**A5 检测到就明确报 `-EOPNOTSUPP`**,留给后续 |
| 相对 mount namespace | 路径在别的 mntns 里含义不同 | 记录 `mnt_id`(`file->f_path.mnt`),A5 只支持同 mntns |
| 路径含换行等特殊字符 | 会破坏你的调试输出 | 镜像里是长度前缀的,protobuf 安全;只影响 printk |
| overlayfs / bind mount | `d_path` 给出的路径 restore 时可能不可达 | A5 只在同机同 mount 表下支持 |

**A5 的正确态度是:能力边界之外就明确报错,不要「尽力而为」。** 一个报
`-EOPNOTSUPP` 的 dump 比一个产出了错误镜像的 dump 好得多 —— 后者会让你在 restore
阶段调试一个根本不该存在的问题。

### 4.4 unix socket:in-flight 的 fd

unix socket 可以通过 `SCM_RIGHTS` 传递 fd。如果 dump 的瞬间有一个 fd 正在
socket 的接收队列里「飞行中」(已发送、未接收),那个 fd 指向的对象也必须被 dump。

这会让对象图产生**环**:socket A 的队列里有一个指向 socket B 的 fd,而 B 的队列里
有指向 A 的。CRIU 用一个专门的收集-解析两阶段算法处理(`criu/criu/sk-unix.c`)。

**A5 的处理:检测到 in-flight fd 就报 `-EOPNOTSUPP`。** 完整支持它的成本远超
A5 的预算,而且它在真实应用里罕见(主要是 systemd 和容器运行时用)。明确不支持,
写进限制列表。

---

## 5. 如何测试

### 5.1 去重专项测试程序

```c
/* fds-dup.c: construct all three sharing cases so a dump must tell them
 * apart. Case A/B differ only in whether f_pos is shared, which is exactly
 * what a naive "just record the path" dump gets wrong.
 *
 *   gcc -static -O0 -o fds-dup fds-dup.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(void)
{
	int a1, a2, b1, b2;
	off_t p;

	/* Case A: two independent opens of the same path. */
	a1 = open("/tmp/a5-shared", O_RDWR | O_CREAT, 0644);
	a2 = open("/tmp/a5-shared", O_RDWR);
	if (a1 < 0 || a2 < 0)
		return 1;
	if (write(a1, "0123456789", 10) != 10)
		return 1;
	lseek(a1, 3, SEEK_SET);
	/* a2 must still be at 0: independent struct file. */

	/* Case B: dup, so both fds share one struct file and one f_pos. */
	b1 = open("/tmp/a5-shared", O_RDONLY);
	b2 = dup(b1);
	if (b1 < 0 || b2 < 0)
		return 1;
	lseek(b1, 7, SEEK_SET);
	/* b2 must now also report 7. */

	printf("pid=%d a1=%d a2=%d b1=%d b2=%d\n", getpid(), a1, a2, b1, b2);
	p = lseek(a1, 0, SEEK_CUR); printf("a1_pos=%ld\n", (long)p);
	p = lseek(a2, 0, SEEK_CUR); printf("a2_pos=%ld\n", (long)p);
	p = lseek(b1, 0, SEEK_CUR); printf("b1_pos=%ld\n", (long)p);
	p = lseek(b2, 0, SEEK_CUR); printf("b2_pos=%ld\n", (long)p);
	fflush(stdout);

	for (;;) {
		/* Re-verify the sharing relationships still hold. A restore
		 * that turns the dup into two independent opens is caught
		 * here, not by an external check.
		 */
		lseek(b1, 5, SEEK_SET);
		if (lseek(b2, 0, SEEK_CUR) != 5) {
			printf("DUP BROKEN: b2 pos != 5\n");
			fflush(stdout);
			return 2;
		}
		lseek(a1, 3, SEEK_SET);
		lseek(a2, 8, SEEK_SET);
		if (lseek(a1, 0, SEEK_CUR) != 3) {
			printf("INDEPENDENCE BROKEN: a1 moved with a2\n");
			fflush(stdout);
			return 3;
		}
		printf("fds ok\n");
		fflush(stdout);
		sleep(1);
	}
	return 0;
}
```

**这个程序的价值在于它让进程自己检验共享关系。** `DUP BROKEN` 和
`INDEPENDENCE BROKEN` 两条互为反向:前者抓「该共享的没共享」,后者抓
「不该共享的共享了」。只测一个方向的话,一个「把所有 fd 都 dup 到同一个 file」的
错误实现能通过前者。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 3 个常规文件 fd,不同 flags 和 pos | 恢复后 pos 与 flags 全部一致 |
| 2 | `dup` 出的 fd 共享 f_pos | 无 `DUP BROKEN` |
| 3 | 两次独立 open 不共享 f_pos | 无 `INDEPENDENCE BROKEN` |
| 4 | pipe 有 4096 字节未读数据 | 恢复后能完整读出,字节一致 |
| 5 | pipe 读端和写端配对正确 | 恢复后写入能被读出 |
| 6 | pipe 为空 | 恢复后仍是空,不是 EOF |
| 7 | pipe 写端已关闭,读端还有数据 | 恢复后读完数据得到 EOF |
| 8 | unix socket 已连接对 | 恢复后双向通信正常 |
| 9 | unix socket 有未读数据 | 数据完整 |
| 10 | fd 号有空洞(3, 7, 100) | fd 号精确恢复 |
| 11 | `O_TRUNC` 打开的文件 | **恢复后文件内容未被清空** |
| 12 | 已删除文件的 fd | 明确 `-EOPNOTSUPP`,不产出错误镜像 |
| 13 | in-flight `SCM_RIGHTS` | 明确 `-EOPNOTSUPP` |
| 14 | 1024 个 fd | 完成,id 分配无冲突 |
| 15 | `F_SETOWN` 设过的 fd | fown 恢复,信号能收到 |
| 16 | `fcntl` 文件锁 | 明确支持或明确 `-EOPNOTSUPP` |

用例 11 单列,因为它是唯一一个**失败会毁用户数据**的用例。

用例 12/13/16 检验的是「边界之外要明确报错」。**它们和功能用例一样重要** ——
把「不支持」实现成「报错」而不是「产出坏镜像」,是这一步的质量标准之一。

### 5.3 ZDTM 增量

```
zdtm/static/file_fown
zdtm/static/pipe00
zdtm/static/pipe01
zdtm/static/fdt_shared
zdtm/static/dup_fd
zdtm/static/unlink_fstat00
zdtm/static/socket_listen
zdtm/static/socket-tcp   # 预期失败,A5 不做 TCP
```

**故意在 allowlist 之外记录预期失败的测试**,让「哪些还不支持」也是可查的。

---

## 6. 完成标准

- [ ] 16 个用例通过(含 3 个明确报错的边界用例)
- [ ] `criu_objmap` 的 `is_new` 模式在 pipe / regfile / unixsk 三处一致使用
- [ ] A3、A4 的测试全部仍然通过
- [ ] 不支持的能力写进 `docs/steps/A5-fds.md` 本文件的「限制」附录
- [ ] allowlist 增加至少 5 个测试
