# 原理 07 —— 文件描述符与共享对象

> 被引用于:[A5](../steps/A5-fds.md)、[A8](../steps/A8-shared-resources.md)

---

## 1. 一个 fd 是三层间接

```
fd 号 3
  └─→ task->files->fdt->fd[3]     (struct file *)
        └─→ struct file           ← 打开状态:f_pos、f_flags、f_owner
              └─→ struct inode    ← 文件本身:内容、权限、大小
```

这三层各自可以被独立共享,而**每一层的共享意味着完全不同的东西**:

| 共享哪一层 | 怎么产生的 | 可观测后果 |
|---|---|---|
| 整个 `fdt`(fd 表) | `CLONE_FILES`(线程) | 一个线程 `open()`,另一个立刻能用那个 fd 号 |
| 同一个 `struct file` | `dup()`、`fork()` | **文件位置共享**:一个 fd 读了 100 字节,另一个从 100 继续 |
| 同一个 `inode` | 两次 `open()` 同一路径 | 内容相同,但**位置各自独立** |

**中间那层是最容易搞错、后果最微妙的一层。**

考虑:

```c
	int a = open("/tmp/x", O_RDONLY);
	int b = dup(a);           /* 同一个 struct file */
	int c = open("/tmp/x", O_RDONLY);  /* 不同的 struct file,同一个 inode */

	read(a, buf, 100);
	/* 现在 b 从 offset 100 读,c 从 offset 0 读 */
```

如果 dump/restore 把 `b` 处理成「再 `open()` 一次同一个路径」,那么恢复后 `a` 和
`b` 的文件位置**不再联动**。所有功能测试可能都过 —— 直到某个程序依赖这个联动
(shell 的 here-document、日志轮转、任何用 `dup` 做重定向的地方)。

**这和 [03](03-memory-and-vma.md) 里 `MAP_SHARED` 存两份是完全同一类错误:
内容对了,关系错了。**

---

## 2. 镜像格式:两级表达共享

`criu/images/fdinfo.proto`:

```protobuf
message fdinfo_entry {
	required uint32		id	= 1;
	required uint32		flags	= 2;
	required fd_types	type	= 3;
	required uint32		fd	= 4;
	optional string		xattr_security_selinux = 5;
}
```

**`fd` 是 fd 号,`id` 是那个 `struct file` 的 id。** 两条 `fdinfo_entry` 的 `id`
相同 ⇔ 它们是 `dup`。

然后 `file_entry` 描述每个 id 具体是什么:

```protobuf
message file_entry {
	required fd_types		type	= 1;
	required uint32			id	= 2;
	optional reg_file_entry		reg	= 3;
	optional inet_sk_entry		isk	= 4;
	...
	optional pipe_entry		pipe	= 18;
```

**这是一个手写的 tagged union:** `type` 说明该看哪个 optional 字段。20 种 fd 类型
(`fd_types` 枚举,`UND=0` 到 `PIDFD=20`)各有自己的 proto 消息。

`flags` 在 `fdinfo_entry` 里而不在 `file_entry` 里,这个位置是对的:`O_CLOEXEC`
是**fd 的属性**,不是 `struct file` 的属性 —— 两个 `dup` 出来的 fd 可以有不同的
`O_CLOEXEC`。而 `O_RDONLY`/`O_APPEND` 这些是 `struct file` 的属性,存在各自的
type-specific 消息里(比如 `reg_file_entry.flags`)。

**搞混这两处 flags 的症状:`exec` 之后 fd 意外地还开着,或者意外地关了。**

---

## 3. 怎么判断两个 fd 指向同一个 `struct file`

### CRIU:fstat + kcmp 的两级树

用户态看不到 `struct file` 的地址,所以 CRIU 必须问内核。它的做法很值得读,
`criu/criu/kcmp-ids.c:14-51` 的注释:

```
 * Basically OS provides us two ways to distinguish files
 *
 *  - information obtained from fstat call
 *  - shiny new sys_kcmp system call (which may compare the file descriptor
 *    pointers inside the kernel and provide us order info)
 *
 * So, to speedup procedure of searching for shared file descriptors
 * we use both techniques. From fstat call we get that named general file
 * IDs (genid) which are carried in the main rbtree.
 *
 * In case if two genid are the same -- we need to use a second way and
 * call for sys_kcmp.
```

两级:

1. `fstat` 拿到 `(st_dev, st_ino, mnt_id)` 当 **genid**。genid 不同 ⇒ 肯定不是同一个
   `struct file`,不用问内核。
2. genid 相同 ⇒ **可能**是 dup,也可能是两次 `open()` 同一文件。此时才调
   `kcmp(pid, pid, KCMP_FILE, fd1, fd2)`。

`kcmp()` 是 Linux 3.5 加的,**也是为 CRIU 加的**。它比较两个 fd 背后的内核指针,
返回 0(相同)或一个稳定的排序结果(不同)。返回排序而不只是相等,是为了能用它
建平衡树 —— 否则判断 N 个 fd 的相等关系需要 O(N²) 次系统调用。

`criu/criu/file-ids.c:22` 就是这棵树的声明:

```c
DECLARE_KCMP_TREE(fd_tree, KCMP_FILE);
```

### 内核模块:比较指针

```c
	/* Pointer identity is the dedup key. CRIU needs fstat + kcmp() and a
	 * two-level rbtree to approximate this from userspace.
	 */
	id = criu_objmap_get(map, file, &is_new);
```

`struct file *` 就在眼前。**一次哈希表查找,零个系统调用。**

**这是内核模块的第三个真实优势。** 它不只是快 —— CRIU 那套两级树是一个需要小心
维护的近似机制,而指针相等是**定义上就正确**的。

同样的手法用在所有共享对象上:`mm`、`files`、`fs`、`sighand`、shmem 的 `inode`。
`is_new` 出参把「去重」和「每个对象只描述一次」合并成一个操作:

```c
	id = criu_objmap_get(map, file, &is_new);
	write_fdinfo(fd, id);			/* always reference */
	if (is_new)
		write_file_entry(id, file);	/* describe once */
```

### 遍历 fd 表的正确姿势

```c
	/* fdtable can be reallocated when a task expands its fd array, so the
	 * lock (or RCU) is not optional even for a frozen task: another task
	 * sharing this files_struct could still be resizing it.
	 */
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	for (i = 0; i < fdt->max_fds; i++) {
		f = rcu_dereference_raw(fdt->fd[i]);
		if (!f)
			continue;
		...
	}
	spin_unlock(&files->file_lock);
```

**「冻结了所以不用加锁」是错的:** 冻结的是我们要 dump 的那些 task,而
`files_struct` 可能被一个**不在** dump 范围内的 task 共享(比如某个进程 `CLONE_FILES`
了一个我们没在 dump 的线程)。[A5](../steps/A5-fds.md) 把这一点写成硬要求。

而且 `file_lock` 是 spinlock,**持有期间不能睡眠** —— 不能在循环里 `kmalloc(GFP_KERNEL)`
或写镜像文件。正确做法是在锁内只收集 `struct file *`(并 `get_file()` 拿引用),
出锁之后再逐个处理。

---

## 4. 恢复一个 fd:号码本身也是状态

`open()` 返回**最小的可用 fd 号**。你不能指定它。所以恢复 fd 3 的标准手法是:

```
1. open() 得到某个号码 tmp(可能是 7)
2. dup2(tmp, 3)     ← dup2 可以指定目标号码
3. close(tmp)
```

`dup2` 就是那个「指定号码」的机制,和 `clone3(set_tid)` 在 pid 上扮演的角色完全
对应。

### 一个必然出现的冲突

restore 程序自己也有打开的 fd(镜像文件、日志)。如果目标进程需要 fd 3,而
restore 程序的镜像文件正好就在 fd 3 上,`dup2(tmp, 3)` 会**静默关掉那个镜像文件**。

于是下一次读镜像时 `EBADF`,而错误现场离原因很远。

CRIU 的做法是把自己的 fd 全部搬到一个高位区间(`service fd`,
`criu/criu/util.c` 的 `get_service_fd()`,基址由 `service_fd_rlim_cur` 决定,
靠近 `RLIMIT_NOFILE` 上限)。**这是 restore 侧一个必须提前设计、事后无法补救的
细节** —— 一旦覆盖发生,你连报错的能力都没了。

### `dup` 的恢复

同一个 id 的多个 fd:第一个真正 `open()`,其余 `dup2()` 过去。

**顺序要求:必须先建立所有 fd,再把它们移到最终位置。** 否则中间某一步的 `dup2`
可能覆盖掉一个还没被搬走的目标 fd。这是一个典型的「需要两遍」的模式,和会话的
两遍 fork 同源。

---

## 5. pipe:必须两端配对的对象

pipe 是理解「共享对象」最好的例子,因为它的两端**天生在不同的进程里**。

```
父进程: fd 3 (读端)  ┐
                     ├─ 同一个 pipe_inode_info
子进程: fd 4 (写端)  ┘
```

### 镜像表示

`criu/images/pipe.proto`:

```protobuf
message pipe_entry {
	required uint32		id		= 1;
	required uint32		pipe_id		= 2;
	required uint32		flags		= 3 [(criu).hex = true];
	required fown_entry	fown		= 4;
	optional uint32		uid		= 5;
	optional uint32		gid		= 6;
}
```

**两个 id,含义完全不同:**

| 字段 | 含义 |
|---|---|
| `id` | 这个 `struct file` 的 id(和 `fdinfo_entry.id` 对应) |
| `pipe_id` | 这个**管道对象**的 id(内核里就是 `inode->i_ino`) |

读端和写端有**不同的 `id`**(两个 `struct file`)但**相同的 `pipe_id`**(一个管道)。

`flags & O_WRONLY` 区分是哪一端 —— 这就是为什么 `open_pipe()` 里能看到
`pfd[p->pe->flags & O_WRONLY]` 这样的下标运算。

管道里**未读的数据**单独存,`criu/images/pipe-data.proto`:

```protobuf
message pipe_data_entry {
	required uint32	pipe_id		= 1;
	required uint32	bytes		= 2;
	optional uint32 size		= 3;
}
```

按 `pipe_id` 索引 —— 数据属于管道,不属于任何一端。

### 恢复:一个进程建,其余进程接

`criu/criu/pipes.c:284` 的 `open_pipe()`:

```c
	if (!pi->create)
		return recv_pipe_fd(pi, new_fd);

	if (pipe(pfd) < 0) {
		pr_perror("Can't create pipe");
		return -1;
	}
	...
	ret = restore_pipe_data(CR_FD_PIPES_DATA, pfd[1], pi->pe->pipe_id, pd_hash_pipes);
	if (ret)
		return -1;

	list_for_each_entry(p, &pi->pipe_list, pipe_list) {
		int fd = pfd[p->pe->flags & O_WRONLY];

		if (send_desc_to_peer(fd, &p->d)) {
```

流程:

1. 同一个 `pipe_id` 的所有条目里,**恰好一个**被标记 `create`
2. 它调 `pipe()` 一次性得到两端
3. 它把未读数据写回写端(`restore_pipe_data`)
4. 它通过 **unix socket 的 `SCM_RIGHTS`** 把对应的那一端发给每个需要它的进程
5. 其余进程 `recv_pipe_fd()` 接收

**第 4 步是关键:`SCM_RIGHTS` 是唯一能把一个 `struct file` 引用跨进程传递的机制。**
不是「重新打开」——重新打开会得到不同的 `struct file`,pipe 甚至根本没有路径可以
重新打开。

### 本项目的处境

restore 在用户态,所以这套 `SCM_RIGHTS` 机制**直接可用,照抄即可**。
这也是「restore 留在用户态」这个决定的一处回报:如果 restore 在内核里,
「把 fd 传给另一个进程」要自己实现 `fd_install()` 的等价物,而进程间协调会变成
一个内核态的同步问题。

### 测试必须双向

一个 pipe 恢复错了(比如两端接反、或者建了两个独立的 pipe),**单向的测试可能通过**:
写端写进去不报错,读端读不到就阻塞 —— 而一个粗糙的测试可能根本没检查读到了什么。

必须:**A 写 B 读,验证内容;然后反向再来一次。** 这和 [03](03-memory-and-vma.md)
里共享内存必须双向检验是同一个道理。

### 顺带一个内核模块的优势

dump 管道里的未读数据,CRIU 只能用 `tee()`/`splice()` 把数据复制出来 —— 而
`tee()` 有容量限制,超过 pipe 缓冲区大小时要分批,还要小心不要真的消耗掉数据。

内核模块直接读 `pipe_inode_info->bufs[]` 数组:

```c
	/* 5.10 uses head/tail; older kernels use nrbufs/curbuf. */
	for (i = pipe->tail; i != pipe->head; i++) {
		struct pipe_buffer *buf = &pipe->bufs[i & (pipe->ring_size - 1)];
		...
	}
```

**纯读,没有容量限制,不可能意外消耗数据。这是第四个真实优势。**

> **版本陷阱:** `head`/`tail`/`ring_size` 是 5.5 之后的字段;更早的内核用
> `nrbufs`/`curbuf`/`buffers`。又一处锁定 5.10 的理由。

---

## 6. 20 种 fd 类型,按难度分层

`fd_types` 枚举有 20 项。它们的难度差异极大,这决定了迭代顺序:

| 层 | 类型 | 难点 |
|---|---|---|
| **1. 简单** | `REG` | 路径 + flags + pos。**mnt namespace 里的路径解析** |
| | `PIPE`、`FIFO` | 两端配对 + 未读数据 |
| | `EVENTFD`、`TIMERFD`、`SIGNALFD` | 单一状态值,无外部关系 |
| **2. 中等** | `EVENTPOLL` | 引用**别的 fd**,有顺序依赖 |
| | `INOTIFY`、`FANOTIFY` | watch 的目标是路径 |
| | `TTY` | 和会话/进程组耦合 |
| **3. 困难** | `UNIXSK` | 两端配对 + 已排队的消息 + 消息里可能带 fd |
| | `INETSK` | **内核里的 TCP 状态机**,需要 `TCP_REPAIR` |
| | `NETLINKSK`、`PACKETSK` | 同上 |
| **4. 放弃** | `EXT` | 定义上就是「交给外部处理」 |

**本项目的范围只到第 1 层**(`REG` + `PIPE`),其余明确返回 `-EOPNOTSUPP`。
理由不是难度,是**可测试性**:

- `INETSK` 需要 `TCP_REPAIR`,而 `TCP_REPAIR` 的正确性验证需要真实的对端
- `EVENTPOLL` 的顺序依赖需要 fd 恢复框架已经稳定
- 而这些的失败模式往往是「连接静默地半死」——**测试通过但语义错了**

**一个明确的 `-EOPNOTSUPP` 比一个静默错误的实现好得多。** 这是本项目在
scope 上反复使用的判据。

---

## 7. `struct file` 上还有什么容易漏

| 字段 | 用户态可见形式 | 漏掉的症状 |
|---|---|---|
| `f_pos` | `lseek(fd, 0, SEEK_CUR)` | 从头/从错误位置读 |
| `f_flags` | `fcntl(F_GETFL)` | `O_APPEND` 丢失 → 覆盖写而不是追加 |
| `f_owner` | `fcntl(F_GETOWN)` | `SIGIO` 发不到正确的进程 |
| `f_mode` | —— | 内部字段,由 open flags 决定,不单独存 |

`f_owner` 对应 proto 里的 `fown_entry`,**每种 fd 类型的消息里都有它** —— 
`pipe_entry.fown`、`reg_file_entry.fown`。它容易被漏掉,因为绝大多数程序不用
`F_SETOWN`,所以漏了也测不出来。**存它的成本是几行代码,不存的代价是一个只在特定
程序上出现的 bug。**

---

## 8. 延伸阅读

- `include/linux/fdtable.h` —— `files_struct`、`fdtable`、`files_fdtable()`
- `include/linux/fs.h` 的 `struct file` —— **看一遍就知道有哪些字段要存**
- `fs/pipe.c` 的 `pipe_inode_info` 和 `pipe_buffer` —— 管道的实际数据结构
- `kernel/kcmp.c` —— `kcmp()` 的实现,看它怎么把指针变成稳定的排序
- `criu/criu/kcmp-ids.c:14-51` —— 那段解释两级树的注释,**是理解 CRIU 去重策略的
  最佳入口**
- `criu/criu/file-ids.c` —— genid 的分配
- `criu/criu/files.c` —— fd 恢复的主框架
- `criu/criu/pipes.c:284` —— `open_pipe()`
- `criu/images/fdinfo.proto`、`pipe.proto`、`pipe-data.proto`、`regfile.proto`
- `man 2 kcmp`、`man 3 cmsg`(`SCM_RIGHTS` 的用法)
- [03-memory-and-vma](03-memory-and-vma.md) —— 共享内存,同一类问题的另一个面
- [04-image-format](04-image-format.md) —— 两级间接的格式细节
