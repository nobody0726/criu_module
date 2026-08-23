# A7 —— 进程树 + session / 进程组

**工期:** 1-2 周 · **前置:** A3 · **产出:** 多进程 `pstree.img`

> 相关原理:[06-pid-and-session](../principles/06-pid-and-session.md)、
> [09-restore-ordering](../principles/09-restore-ordering.md)

---

## 1. 设计思路

### dump 侧出乎意料地简单

`pstree.proto` 全文只有五个字段:

```protobuf
message pstree_entry {
	required uint32			pid		= 1;
	required uint32			ppid		= 2;
	required uint32			pgid		= 3;
	required uint32			sid		= 4;
	repeated uint32			threads		= 5;
}
```

一张**平表**。没有指针、没有嵌套、没有 `children` 链表。dump 侧的工作就是遍历
进程树,每个进程写一条记录。

内核里这四个数的来源:

```c
	entry.pid  = task_pid_vnr(task);
	entry.ppid = task_pid_vnr(rcu_dereference(task->real_parent));
	entry.pgid = task_pgrp_vnr(task);
	entry.sid  = task_session_vnr(task);
```

**必须用 `*_vnr()` 版本**(virtual number,当前 pid namespace 内的编号),不能用
`task->pid`。用错的症状:在容器里 dump 出来的 pid 是宿主机视角的,restore 到容器
里全部对不上。

### 为什么这一步的重点在「读懂 restore」

A7 的代码量很小,但它是理解整个 C/R 设计的关键一步。原设计大纲把「Session 和
进程组」放在 Phase 3 当成一个后加功能,这是判断错误 —— **它决定了 dump 侧必须
采集什么,以及为什么 restore 必须是那个形状。**

核心事实:**进程树不是被「重建」的,是被「长」出来的。**

restore 时没有任何代码去 malloc `task_struct` 然后互相填指针。取而代之的是每个
task 自己 fork 出自己的孩子:

- 进程 1 起来 → fork 出图纸上它的孩子 → 得到进程 2、3
- 进程 2 起来 → fork 出它的孩子 → 得到进程 4
- 递归到底

于是 `real_parent`、`children`、`sibling`、`thread_group` **全部由内核在
`copy_process()` 里自然填好**。这就是为什么 `pstree.proto` 里没有这些字段 —— 
它们不需要被存储,因为它们不需要被恢复。

**`ppid` 存在平表里只是给 restore 端拼装「施工图纸」用的。restore 时不需要任何
`setppid()` 系统调用 —— Linux 根本没有这个系统调用,而正是因为不需要,才没有。**

### 唯一真正麻烦的是 session

`setsid()` 有两个恶劣性质:

1. **不可逆。** 开了新 session 回不去。
2. **没有「加入已有 session」的系统调用。** 只能新建。

于是恢复 session 变成一个排序问题:如果孩子和父亲不在同一个 session,孩子必须在
父亲调 `setsid()` **之前**就被 fork 出来 —— 因为 fork 继承的是父亲**当时**的
session。

这就是 A7 在 dump 侧必须多采集一个东西的原因:**`born_sid`** —— 这个孩子出生时
父亲在哪个 session。

而 `pgid` 反而简单:进程组**可以加入**(只要组长存在),所以不依赖 fork 顺序,
只需要等组长就位。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 两趟 fork

`criu/criu/cr-restore.c:1491` 的 `create_children_and_session()`:

```c
	pr_info("Restoring children in alien sessions:\n");
	list_for_each_entry(child, &current->children, sibling) {
		if (!restore_before_setsid(child))
			continue;
		BUG_ON(child->born_sid != -1 && getsid(0) != child->born_sid);
		ret = fork_with_pid(child);
		if (ret < 0)
			return ret;
	}

	if (current->parent)
		restore_sid();

	pr_info("Restoring children in our session:\n");
	list_for_each_entry(child, &current->children, sibling) {
		if (restore_before_setsid(child))
			continue;
		ret = fork_with_pid(child);
		if (ret < 0)
			return ret;
	}
```

结构是:**第一趟 fork「必须在 setsid 之前出生」的孩子 → 自己 `setsid()` →
第二趟 fork 剩下的**。那个 `BUG_ON` 是在断言施工顺序没排错。

### 2.2 `restore_sid()`:leader 动手,其余人只校验

`criu/criu/cr-restore.c:1377`:

```c
	if (vpid(current) == current->sid) {
		sid = setsid();
		if (sid != current->sid) {
			pr_perror("Can't restore sid (%d)", sid);
			exit(1);
		}
	} else {
		sid = getsid(0);
		if (sid != current->sid) {
			...
			exit(1);
		}
	}
```

`vpid(current) == current->sid` 就是「我是不是 session leader」。**只有 leader 调
`setsid()`**,而 `setsid()` 的语义是「新 sid = 调用者的 pid」—— 既然 pid 已经被
`clone3(set_tid)` 钉对了,`setsid()` 返回的值自动就是对的。

非 leader 走 `else` 分支,**里面一个系统调用都没有**,只有 `getsid(0)` 读出来
对一遍。这就是构造式恢复的样子:**正确的东西不需要动作,只需要断言。**

### 2.3 `restore_pgid()`:等组长,然后加入

`criu/criu/cr-restore.c:1396` 的注释点明了与 session 的区别:

> Unlike sessions, process groups (a.k.a. pgids) can be joined by any task,
> provided the task with pid == pgid (group leader) exists.

实现:

```c
		leader = rsti(current)->pgrp_leader;
		if (leader) {
			BUG_ON(my_pgid != vpid(leader));
			futex_wait_until(&rsti(leader)->pgrp_set, 1);
		}
	...
	if (setpgid(0, my_pgid) != 0) { ... exit(1); }
	if (my_pgid == vpid(current))
		futex_set_and_wake(&rsti(current)->pgrp_set, 1);
```

组长 `setpgid` 成功后 `futex_set_and_wake` 通知,组员 `futex_wait_until` 等它。
这是**补偿式恢复**必须显式排序的样本,而跨进程排序就得靠共享内存里的 futex。

### 2.4 `TASK_HELPER`:施工脚手架

如果拓扑更扭曲(比如某个 session 的 leader 在 checkpoint 时已经死了,但 session
还在,还有别的进程属于它),两趟也不够。CRIU 会插入 `TASK_HELPER` —— 纯粹为了
摆出正确的 session/组拓扑而存在的临时进程,摆好之后退出。

它在 checkpoint 时不存在,在 restore 后也不存在,只在施工期间存在。像脚手架。

**A7 的 dump 侧需要检测这种情况并记录足够信息**,让 restore 端知道要插 helper。
检测条件:某个 sid 或 pgid 对应的 leader 不在被 dump 的进程集合里。

### 2.5 根任务的 ppid 是无法恢复的

树里所有**非根**任务的 `ppid` 都精确恢复,因为它们就是被正确的父亲 fork 出来的。
但根任务是 CRIU 自己 fork 的,`ppid` 会是 CRIU 的 pid,restore 结束后被 reparent
到 init 或最近的 subreaper。

`criu/criu/cr-restore.c:1040` 有一个部分缓解:

```c
	if (opts.restore_sibling) {
		...
		rsti(item)->clone_flags |= CLONE_PARENT;
```

`CLONE_PARENT` 让新任务的父亲是**调用者的父亲**而不是调用者。但这只是把根任务挂到
调用者的父亲上,**不是恢复成原来那个 ppid** —— 原来那个父亲根本不在被 dump 的
树里,你没有任何合法途径把一个新进程塞给它当孩子。

**这个事实说明了为什么 checkpoint 必须按「进程树」而不是「单进程」为单位。**
A7 的 dump 侧要做的是:检测「被 dump 集合的根的父亲在集合之外」这个情况,并在
镜像的元信息里标记,而不是假装它能被恢复。

**要抄的:** 平表格式、`born_sid` 的采集、helper 检测。
**不抄的:** 两趟 fork 本身(那是 restore 侧,B2 做)。

---

## 3. 文件结构

**Create:**
- `kernel_module/checkpoint/dump_pstree.c`
- `kernel_module/checkpoint/collect_tree.c` —— 收集进程集合 + 拓扑校验
- `tests/progs/tree-simple.c` / `tree-session.c` / `tree-zombie.c`

**Modify:**
- `kernel_module/checkpoint/dump.c` —— 从单进程改为遍历整棵树
- `kernel_module/checkpoint/freeze.c` —— `include_children` 参数真正生效

**Interfaces produced:**

```c
/* One node of the collected tree. This mirrors pstree_entry plus the extra
 * ordering information the restore side needs but the proto does not carry
 * directly. */
struct criu_pstree_node {
	pid_t			vpid;
	pid_t			vppid;
	pid_t			vpgid;
	pid_t			vsid;
	/* Session the parent was in when this child was forked. -1 when the
	 * child was never in a different session than its parent. This is
	 * what lets the restore side decide which fork pass a child belongs
	 * to (see CRIU's restore_before_setsid()). */
	pid_t			born_sid;
	struct task_struct	*task;		/* pinned */
	struct list_head	siblings;
	struct list_head	children;
	pid_t			*tids;
	int			nr_tids;
	bool			is_zombie;
};

/* Collect the whole descendant set of root_vpid. The set is closed under
 * "parent of", so every ppid in the result is either in the set or is the
 * root's own parent. */
int criu_collect_pstree(pid_t root_vpid, struct criu_pstree_node **root_out);
void criu_free_pstree(struct criu_pstree_node *root);

/* Topology validation. Returns 0 if the collected set can actually be
 * restored, or -EOPNOTSUPP with a printk explaining which constraint fails.
 * Call this BEFORE writing any images: producing an unrestorable image set is
 * worse than refusing to dump. */
int criu_validate_topology(struct criu_pstree_node *root);
```

`criu_validate_topology` 单独成一个接口、并且要求在写任何镜像之前调用,是 A7 最
重要的设计决定。**产出一份无法恢复的镜像,比拒绝 dump 糟糕得多** —— 后者的错误
信息指向真正的原因,前者会让你在 restore 阶段调试一个本来就不可能成功的操作。

---

## 4. 关键实现要点

### 4.1 遍历子孙

```c
	/* Walk children recursively. tasklist_lock (read) is enough and is
	 * what the kernel's own do_each_thread users take; RCU also works for
	 * a read-only walk. Tasks are already frozen, so the tree is stable.
	 */
	read_lock(&tasklist_lock);
	list_for_each_entry(child, &task->children, sibling) {
		/* child is a task_struct; recurse */
	}
	read_unlock(&tasklist_lock);
```

`tasklist_lock` 是否可从模块访问要在 S0 验证(它是导出的 `rwlock_t`,但曾经
有过变化)。退路是纯 RCU 遍历。

**不要递归调用**:一棵深 10000 层的进程树会爆内核栈(内核栈只有 16KB)。用显式
栈或工作队列。这不是理论风险 —— `fork` 循环很容易造出极深的树,测试用例 9 会造。

### 4.2 `born_sid` 怎么采集

这是 A7 唯一一个「内核里没有现成字段」的信息。内核不记录「这个 task 出生时它
父亲在哪个 session」。

可用的推断:如果 `child->sid != parent->sid`,而且 child 不是自己 session 的
leader(`child->pid != child->sid`),那么 child 必然是在 parent `setsid()`
**之前**出生的 —— 否则它会继承 parent 的新 session。

```c
	/* The kernel does not record "which session was my parent in when I was
	 * forked", so derive it. A child whose sid differs from its parent's,
	 * and which is not its own session leader, must have been forked before
	 * the parent called setsid() -- otherwise it would have inherited the
	 * parent's new session.
	 */
	if (child_sid != parent_sid && child_vpid != child_sid)
		node->born_sid = child_sid;
	else
		node->born_sid = -1;
```

这个推断在常见拓扑下正确。**不覆盖的情况**(比如 child 自己也 `setsid()` 过,
中间还有别的进程已经退出)要由 `criu_validate_topology` 检测并拒绝。
诚实地把推断的边界写在注释里,比假装它总是对的更有价值。

### 4.3 僵尸进程

僵尸(`EXIT_ZOMBIE`)必须 dump,因为父进程还没 `wait()`,它的退出码还是可观测
状态。但僵尸没有 `mm`、没有 fd 表 —— 它只有一个退出码。

```c
	/* A zombie has already released its mm and fd table; only the exit
	 * code survives, and only until the parent reaps it. Do not try to
	 * dump memory or files for one.
	 */
	if (task->exit_state == EXIT_ZOMBIE) {
		node->is_zombie = true;
		/* task->exit_code is what wait() will report */
		return 0;
	}
```

`get_task_mm()` 对僵尸返回 NULL —— A1 的用例 5 已经覆盖了这条路径不 oops。
**这是早期步骤的错误路径测试在后续步骤兑现价值的例子。**

### 4.4 拓扑校验要检查什么

| 检查 | 失败时 |
|---|---|
| 每个 `ppid` 在集合内,或等于根的父亲 | `-EOPNOTSUPP`:集合不闭合 |
| 每个 `sid` 的 leader 在集合内,或该 sid 无人是 leader(需 helper) | 记录需要 helper |
| 每个 `pgid` 的 leader 在集合内 | 记录需要 helper |
| 根的父亲在集合外 | 记录「ppid 不可恢复」警告(**不是错误**) |
| 无环 | `-EINVAL`(不该发生,但值得断言) |
| 深度 < 1000 | `-E2BIG`,避免 restore 端递归 fork 爆栈 |

---

## 5. 如何测试

### 5.1 session 拓扑测试程序

```c
/* tree-session.c: build a process tree with a deliberately awkward session
 * layout -- one child in the parent's session, one in its own -- so a restore
 * that gets the fork ordering wrong cannot succeed by accident.
 *
 *   parent (sid=P)
 *     |- child A (sid=P)      forked AFTER parent's setsid
 *     |- child B (sid=B)      calls setsid itself
 *     |    `- grandchild (sid=B)
 *     `- child C (sid=old)    forked BEFORE parent's setsid
 *
 *   gcc -static -O0 -o tree-session tree-session.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static void report(const char *who)
{
	printf("%s pid=%d ppid=%d pgid=%d sid=%d\n",
	       who, getpid(), getppid(), getpgrp(), getsid(0));
	fflush(stdout);
}

static void spin(const char *who)
{
	unsigned long t = 0;

	for (;;) {
		/* Re-report periodically: after a restore these values must be
		 * identical, and the process itself is the best witness. */
		printf("%s alive t=%lu pid=%d ppid=%d pgid=%d sid=%d\n",
		       who, t++, getpid(), getppid(), getpgrp(), getsid(0));
		fflush(stdout);
		sleep(1);
	}
}

int main(void)
{
	pid_t c;

	/* Child C is forked FIRST, before we change session, so it stays in
	 * our original session. This is the case that forces two fork passes
	 * on restore. */
	c = fork();
	if (c == 0) {
		report("C");
		spin("C");
	}

	if (setsid() < 0) {
		/* Already a group leader: re-exec via a fork so setsid works. */
		perror("setsid");
		return 1;
	}
	report("parent");

	/* Child A inherits our NEW session. */
	c = fork();
	if (c == 0) {
		report("A");
		spin("A");
	}

	/* Child B makes its own session, and has a child of its own in it. */
	c = fork();
	if (c == 0) {
		if (setsid() < 0)
			return 1;
		report("B");
		c = fork();
		if (c == 0) {
			report("grandchild");
			spin("grandchild");
		}
		spin("B");
	}

	spin("parent");
	return 0;
}
```

**这棵树的形状是刻意设计的:child C 在 `setsid()` 之前 fork,A 在之后。** 一个
不区分两趟 fork 的 restore 实现无法同时把 A 和 C 放对 —— 它会在 `restore_sid()`
的那个 `exit(1)` 上失败。这就是本测试的判定力所在。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 父 + 3 子 的平坦树 | 全部恢复,`ppid` 全对 |
| 2 | 5 层深的链式树 | 全部恢复 |
| 3 | `tree-session.c` 的混合 session 拓扑 | 全部恢复,每进程 4 个数全对 |
| 4 | 所有进程的 `sid` 一致 | `getsid` 输出一致 |
| 5 | 所有进程的 `pgid` 一致 | `getpgrp` 输出一致 |
| 6 | 僵尸子进程 | 恢复后父进程 `wait()` 拿到**相同的退出码** |
| 7 | 孤儿进程(父已退出,被 init 收养) | 明确处理或明确报错 |
| 8 | session leader 已死但 session 仍有成员 | 检测到需要 helper,或明确 `-EOPNOTSUPP` |
| 9 | 深度 2000 的树 | `-E2BIG`,**不爆内核栈** |
| 10 | 1000 个兄弟进程 | 完成,不超时 |
| 11 | dump 期间有进程 fork | 冻结先行 ⇒ 不发生;断言不触发 |
| 12 | dump 期间有子进程退出成僵尸 | 处理正确,不 oops |
| 13 | 集合不闭合(指定的根有外部父亲) | 警告但继续(`ppid` 不可恢复) |
| 14 | 进程组跨越树的多个分支 | `pgid` 全部正确 |

用例 6 的构造:子进程 `exit(42)`,父进程不 `wait()`,checkpoint,restore,
然后父进程 `wait()` —— **必须拿到 42**。这是僵尸 dump 唯一有意义的验证方式。

用例 9 是安全用例:它验证的不是功能,而是**你的实现不会因为输入畸形而崩内核**。
构造:

```c
/* deep-tree.c: fork a 2000-level chain. Each level keeps only one child, so
 * the tree is a path, not a bush. Tests that the collector does not recurse. */
	for (i = 0; i < 2000; i++) {
		if (fork() != 0)
			break;	/* parent stops here; only the child continues */
	}
```

### 5.3 ZDTM 增量

```
zdtm/static/pstree
zdtm/static/session00
zdtm/static/session01
zdtm/static/session02
zdtm/static/zombie00
zdtm/static/pgrp00
```

`zdtm/static/session0*` 是最有价值的一组 —— ZDTM 里这几个测试专门覆盖了本步骤
最难的拓扑。

---

## 6. 完成标准

- [ ] 14 个用例通过,含 6、8、9 三个难点
- [ ] `criu_validate_topology` 在写任何镜像**之前**被调用(代码审查确认)
- [ ] 收集器用显式栈或队列,**无递归**(用例 9 验证)
- [ ] `born_sid` 推断的边界写进代码注释
- [ ] A3-A6 测试全部仍通过
- [ ] allowlist 增加至少 4 个测试,含 2 个 session 测试
