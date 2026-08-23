# B2 —— 用户态 restore:进程树

**工期:** 2-3 周 · **前置:** B1 · **产出:** 多进程 + session/pgid 的恢复

> 相关原理:[06-pid-and-session](../principles/06-pid-and-session.md)、
> [09-restore-ordering](../principles/09-restore-ordering.md)

---

## 1. 设计思路

### 从「一个进程」到「一棵树」只增加了排序问题

B1 已经把单个进程恢复的全部机制建好了:`clone3(set_tid)`、B 类恢复、
premap/mremap、`rt_sigreturn`。B2 不引入任何新机制,它引入的是**顺序**。

要解决的排序问题有三个:

| 问题 | 约束来源 | 解法 |
|---|---|---|
| 谁来 fork 谁 | `ppid` 只能靠继承 | 每个 task 自己 fork 自己的孩子(递归) |
| session 怎么摆 | `setsid()` 不可逆、无法加入 | 两趟 fork,`restore_sid()` 夹在中间 |
| pgid 怎么摆 | 需要组长先存在 | futex 等组长,然后 `setpgid` |
| 所有人何时一起 `rt_sigreturn` | 谁先谁后都可能出问题 | 全局屏障 |

**注意这四个约束里没有一个能靠「重试」解决。** 这与 dump 侧形成鲜明对比:
dump 是纯读,顺序错了重来一遍就行;restore 每一步都在消耗不可逆的资源。

### 递归 fork:为什么这是唯一的形状

已经在 A7 里论证过,这里重复关键结论,因为 B2 是它的实现方:

**Linux 没有 `setppid()`。** 一个进程的父亲只能在它被创建的那一刻确定。所以要让
恢复后的进程树关系正确,唯一的办法是**让正确的父亲去创建它**。

于是 restore 的形状必然是:

```
mini-restore 主进程
  └─ clone3(set_tid=root_pid) ──► 根任务
        ├─ clone3(set_tid=c1) ──► 孩子 1
        │     └─ clone3(set_tid=g1) ──► 孙子 1     ← 由孩子 1 创建,不是根创建
        └─ clone3(set_tid=c2) ──► 孩子 2
```

`real_parent`、`children`、`sibling` 三个链表**全部由内核在 `copy_process()` 里
自然填好**。B2 一行填指针的代码都不用写。

**这就是「构造式恢复」:创建得对,关系自动就对。**

### 两趟 fork 的必要性

考虑这棵树:

```
根任务 R (sid = S1)
  ├─ 孩子 A (sid = S1)   —— 和 R 同 session
  └─ 孩子 C (sid = S0)   —— 在 R 调 setsid() 之前出生的,还留在老 session
```

如果 R 先 `setsid()` 再 fork 两个孩子,两个孩子都会在 S1 里,C 就错了。
如果 R 先 fork 两个孩子再 `setsid()`,两个孩子都会在 S0 里,A 就错了。

**没有任何单趟顺序能同时满足。** 所以必须:

```
1. fork 那些「必须在 setsid 之前出生」的孩子   ← C
2. setsid()
3. fork 剩下的孩子                            ← A
```

`born_sid`(A7 采集的那个字段)就是用来区分这两组的。

**这个两趟结构不是优化,是逻辑必需。** 理解了它,就理解了为什么 CRIU 的 restore
流程看起来那么繁琐 —— 它不是设计得不好,是约束本身就这么硬。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 递归 fork 的入口

`criu/criu/cr-restore.c:1491` 的 `create_children_and_session()`,完整结构:

```c
	pr_info("Restoring children in alien sessions:\n");
	list_for_each_entry(child, &current->children, sibling) {
		if (!restore_before_setsid(child))
			continue;
		BUG_ON(child->born_sid != -1 && getsid(0) != child->born_sid);
		ret = fork_with_pid(child);		/* line 1503 */
		if (ret < 0)
			return ret;
	}

	if (current->parent)
		restore_sid();

	pr_info("Restoring children in our session:\n");
	list_for_each_entry(child, &current->children, sibling) {
		if (restore_before_setsid(child))
			continue;
		ret = fork_with_pid(child);		/* line 1516 */
		if (ret < 0)
			return ret;
	}
```

两个循环用 `restore_before_setsid()` 的返回值互补地过滤同一个 `children` 链表。
中间那个 `BUG_ON` 断言「我现在所处的 session 确实是这个孩子该出生的 session」。

`fork_with_pid()` 在 `criu/criu/cr-restore.c:1097`。它 fork 出的孩子会执行
`restore_task_with_children()`,后者又会调 `create_children_and_session()` —— 
**递归就是这么闭合的。**

`criu/criu/cr-restore.c:2117` 的 `fork_with_pid(init)` 是整棵树的起点。

### 2.2 `restore_sid()`

`criu/criu/cr-restore.c:1364` 起。这个函数值得完整引用,因为它把「构造式」和
「补偿式」的分界画得非常清楚:

```c
static void restore_sid(void)
{
	pid_t sid;
	/*
	 * SID can only be reset to pid or inherited from parent.
	 * Thus we restore it right here to let our kids inherit
	 * one in case they need it.
	 *
	 * PGIDs are restored late when all tasks are forked and
	 * we can call setpgid() on custom values.
	 */
	if (vpid(current) == current->sid) {
		pr_info("Restoring %d to %d sid\n", vpid(current), current->sid);
		sid = setsid();
		if (sid != current->sid) {
			pr_perror("Can't restore sid (%d)", sid);
			exit(1);
		}
	} else {
		sid = getsid(0);
		if (sid != current->sid) {
			/* Skip the root task if it's not init */
			if (current == root_item && vpid(root_item) != INIT_PID)
				return;
			pr_err("Requested sid %d doesn't match inherited %d\n",
			       current->sid, sid);
			exit(1);
		}
	}
}
```

三条要点:

1. **只有 session leader 调 `setsid()`。** 判据是 `vpid(current) == current->sid`
   —— 一个 session 的 id 就是它 leader 的 pid,这是 sid 能被恢复的根本原因。
2. **非 leader 只校验,不动作。** `else` 分支里没有系统调用,只有一个 `getsid(0)`
   读回来对一遍。正确的东西不需要动作。
3. **注释里那句「PGIDs are restored late」是整个排序设计的说明。** sid 必须早
   (因为孩子要继承),pgid 必须晚(因为要等组长)。

### 2.3 `restore_pgid()`

`criu/criu/cr-restore.c:1396` 起。注释点明与 session 的关键区别:

> Unlike sessions, process groups (a.k.a. pgids) can be joined by any task,
> provided the task with pid == pgid (group leader) exists.

```c
		leader = rsti(current)->pgrp_leader;
		if (leader) {
			BUG_ON(my_pgid != vpid(leader));
			futex_wait_until(&rsti(leader)->pgrp_set, 1);
		}
	...
	if (setpgid(0, my_pgid) != 0) {
		pr_perror("Can't restore pgid (%d/%d->%d)", vpid(current),
			  getpgid(0), current->pgid);
		exit(1);
	}

	if (my_pgid == vpid(current))
		futex_set_and_wake(&rsti(current)->pgrp_set, 1);
```

**「可加入」把一个 fork 顺序问题降级成了一个等待问题。** 于是它不需要影响树的
构造方式,只需要一个跨进程的同步原语。

### 2.4 跨进程同步靠共享内存里的 futex

`futex_wait_until` / `futex_set_and_wake` 操作的是 `rsti(item)` 里的字段,而
`rsti()` 指向的结构在一块**所有 restore 参与者共享的匿名 `MAP_SHARED` 内存**里
(在 fork 之前建立,所以所有后代都继承了它)。

这是 B2 必须建立的基础设施:

```c
	/* All restore participants need a shared scratch area for barriers.
	 * Map it MAP_SHARED before the first clone so every descendant inherits
	 * the same physical pages -- this is the only channel available, since
	 * the tasks are separate processes with separate address spaces.
	 */
	shared = mmap(NULL, shared_len, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
```

**必须在第一次 clone 之前 mmap。** 之后再 mmap 就不共享了。这是一个「顺序错了
就完全不工作,而且症状是死锁而非报错」的约束。

### 2.5 `TASK_HELPER`:摆拓扑用的临时进程

某些拓扑连两趟 fork 也摆不出来。典型情况:一个 session 的 leader 在 checkpoint
时已经退出了,但 session 还在,还有别的进程属于它。

那个 sid 对应的 pid 现在没有任何存活进程占用,可是「进入这个 session」需要有人
以那个 pid 调 `setsid()`。CRIU 的解法:**造一个 `TASK_HELPER`** —— 一个用那个
pid 创建、调完 `setsid()`、等该进的进程都进来之后就退出的临时进程。

它在 checkpoint 时不存在,restore 完成后也不存在,只在施工期间存在。像脚手架。

**B2 的实现顺序建议:先不做 helper,让这类拓扑明确报错。** 理由:helper 的实现
需要额外的生命周期管理(什么时候退出、谁 `wait` 它),而它覆盖的是罕见拓扑。
先把常见情况做扎实,再回来补。A7 的 `criu_validate_topology` 已经在 dump 侧
检测出这类情况了,B2 只要在 restore 侧同样明确拒绝就一致了。

### 2.6 全局屏障:所有人一起 `rt_sigreturn`

所有任务都恢复好之后,需要一个屏障让它们同时进入目标状态。原因:

- 如果 A 先 `rt_sigreturn` 而 B 还没恢复完,A 可能立刻尝试和 B 通信(通过 pipe、
  共享内存、信号),而 B 还是 restore 程序的状态
- pgid 恢复必须在所有 task 都 fork 出来之后(组长得存在)

CRIU 用 `CR_STATE_*` 一串状态和 `restore_wait_other_tasks()` 实现。B2 的极简版
可以用同一块共享内存里的一个计数器 + futex:

```c
	/* Barrier: every task increments, then waits until the count reaches the
	 * total. Nobody rt_sigreturns until everybody is ready, otherwise an
	 * early starter can try to talk to a task that is still restore code.
	 */
	__sync_fetch_and_add(&shared->ready_count, 1);
	futex_wait_until(&shared->ready_count, shared->total_tasks);
```

**这个屏障必须在 `rst_finalize` 之前**,因为 `rst_finalize` 之后自己的代码就没了,
没法再等任何人。

---

## 3. 文件结构

**Create:**
- `userspace/mini-restore/rst_pstree.c` —— 读 `pstree.img`,建施工图纸
- `userspace/mini-restore/rst_fork.c` —— 递归 fork + 两趟划分
- `userspace/mini-restore/rst_session.c` —— `restore_sid` / `restore_pgid`
- `userspace/mini-restore/rst_shared.c` —— 共享 scratch 区 + futex 屏障
- `tests/b2-restore.sh`

**Modify:**
- `userspace/mini-restore/main.c` —— 从「恢复一个进程」改为「恢复一棵树」
- `userspace/mini-restore/task_create.c` —— 复用 B1 的 `clone3` 包装

**Interfaces produced:**

```c
/* One node of the restore blueprint, built from pstree.img. */
struct rst_item {
	pid_t			pid;
	pid_t			ppid;
	pid_t			pgid;
	pid_t			sid;
	pid_t			born_sid;	/* -1 if same as parent's */
	struct rst_item		*parent;
	struct list_head	 children;
	struct list_head	 siblings;
	struct rst_ctx		*ctx;		/* per-task images, see B1 */
	/* Index into the shared scratch area. Not a pointer: this struct lives
	 * in each task's private memory, so cross-task state must be reached
	 * through the shared mapping instead. */
	unsigned		 shared_idx;
};

/* Shared scratch area. Mapped MAP_SHARED before the first clone, so every
 * task sees the same physical pages. All cross-task synchronisation goes
 * through here -- there is no other channel between separate address spaces. */
struct rst_shared {
	unsigned	total_tasks;
	unsigned	ready_count;		/* barrier counter */
	unsigned	pgrp_set[0];		/* one futex word per task */
};

int rst_read_pstree(const char *img_dir, struct rst_item **root_out,
		    unsigned *nr_tasks_out);

/* Map the shared area. MUST be called before the first clone. */
int rst_shared_init(unsigned nr_tasks, struct rst_shared **out);

/* Does this child have to be forked before we call setsid()? True when the
 * child belongs to a different session than the one we will end up in. */
bool rst_before_setsid(const struct rst_item *child);

/* Recursive: forks our children in two passes with restore_sid in between,
 * then continues into the per-task restore from B1. Called in each task. */
int rst_create_children_and_session(struct rst_item *item);

int rst_restore_sid(struct rst_item *item);
int rst_restore_pgid(struct rst_item *item, struct rst_shared *sh);

/* Barrier. Every task calls this after its own B-class restore is complete and
 * before rst_finalize. */
void rst_wait_all_ready(struct rst_shared *sh);
```

`struct rst_item` 里那个 `shared_idx` 而不是指针,是 B2 最容易搞错的一处:
**每个 task 的 `rst_item` 是各自私有内存里的副本**(fork 之后就分岔了),跨 task
的状态必须通过 `rst_shared` 访问。用指针会「看起来能编译、单进程时能跑、
多进程时静默不同步」。

---

## 4. 关键实现要点

### 4.1 `rst_before_setsid` 的判据

```c
bool rst_before_setsid(const struct rst_item *child)
{
	/* A child that will end up in a different session than we will must be
	 * forked before our setsid(), because fork inherits the session as it is
	 * at fork time and there is no way to join a session afterwards.
	 */
	if (child->born_sid != -1)
		return true;
	return child->sid != child->parent->sid;
}
```

两个条件都要:`born_sid` 是 dump 侧的显式记录(A7 采集),`sid` 比较是兜底。

### 4.2 fork 之后立刻分岔

```c
	pid = rst_clone_with_pid(child->pid, restore_task_fn, child);
	if (pid < 0)
		return -1;
	/* Parent continues the loop; the child never returns here -- it goes
	 * into restore_task_fn, which recurses into
	 * rst_create_children_and_session for its own children.
	 */
```

**孩子不返回到这个循环里。** 它进入自己的恢复流程。这个控制流看起来简单,但
写错的话(比如用 `fork()` 的返回值判断而漏了一个分支)会导致某个 task 既 fork
了自己的孩子又继续 fork 了兄弟的孩子,树的形状就错了。

### 4.3 pgid 恢复必须在屏障之后?不是

顺序是:

```
1. 全部 fork 完(递归自然完成)
2. 各自恢复 B 类(fd、信号、creds ...)
3. restore_pgid()          ← 需要组长存在,而组长在第 1 步就存在了
4. 屏障:等所有人就绪
5. rst_finalize() → rt_sigreturn
```

`restore_pgid` 在第 3 步而不是第 4 步之后,因为它自己内部就有 futex 等待
(等组长的 `pgrp_set`)。**把它放到全局屏障之后是多余的,放到之前反而利用了
细粒度的等待。**

但注意:`restore_pgid` 有自己的死锁风险 —— 如果组长因为别的原因失败退出了,
等它的人会永久卡住。加超时:

```c
	/* Time out rather than hang forever: if the group leader died during its
	 * own restore, waiting is pointless and a hung restore is much harder to
	 * diagnose than a failed one.
	 */
	if (futex_wait_until_timeout(&sh->pgrp_set[leader_idx], 1, 30) < 0) {
		fprintf(stderr, "pgid %d: leader %d never became ready\n",
			my_pgid, leader_pid);
		return -1;
	}
```

**「超时报错」优于「永久挂住」** 在整个 B 轨都成立,因为 restore 的失败模式里
死锁是最难诊断的一类。

### 4.4 根任务的 ppid

B1 已经知道这一点,B2 要正式处理:**根任务的 ppid 无法恢复。** 它的父亲是
mini-restore 进程。

CRIU 的部分缓解在 `criu/criu/cr-restore.c:1040`:

```c
	if (opts.restore_sibling) {
		/*
		 * This means we're called from lib's criu_restore_child().
		 * In that case create the root task as the child one to
		 * the caller. This is the only way to correctly restore the
		 * pdeath_sig of the root task. But also looks nice.
		 * ...
		 */
		rsti(item)->clone_flags |= CLONE_PARENT;
```

`CLONE_PARENT` 让新任务的父亲是**调用者的父亲**。这能把根任务挂到 mini-restore
的父亲(通常是 shell)上,但**不是恢复成原来那个 ppid** —— 原来那个父亲不在被
恢复的集合里。

**B2 的处理:实现 `--restore-sibling` 等价选项,并在文档里明确写「根任务的 ppid
不可恢复」。** 这是一个真实的、无法绕过的语义缺口,诚实地记录它比假装它不存在
更有价值。

### 4.5 mini-restore 主进程要 `wait` 谁

只有根任务是它的孩子(除非用了 `CLONE_PARENT`)。其余任务是孙子及更远,
`wait()` 不到。

```c
	/* We are the parent of the root task only; everyone else is a
	 * descendant of it. Waiting for the root is enough to know the tree is
	 * either up or dead.
	 */
	if (waitpid(root_pid, &status, 0) < 0 && errno != ECHILD)
		...
```

如果用 `--restore-detached`(恢复后 mini-restore 自己退出),根任务会被 reparent
到 init。**这种情况下必须先确认恢复成功再退出**,否则失败了也没人知道。做法:
在共享内存里放一个「恢复完成」标志,主进程等它或超时。

---

## 5. 如何测试

### 5.1 B2 的验收脚本

```sh
#!/bin/sh
# B2 acceptance: restore a real-criu-dumped process tree with our code,
# including the awkward session layout from A7's test program.
set -e

IMGS=/tmp/b2-imgs
rm -rf $IMGS && mkdir -p $IMGS

./tests/progs/tree-session > /tmp/b2.out 2>&1 &
sleep 3
ROOT=$(sed -n 's/^parent pid=\([0-9]*\).*/\1/p' /tmp/b2.out | head -1)
echo "root pid=$ROOT"

# Record every task's four numbers before the checkpoint.
grep -E '^(parent|A|B|C|grandchild) alive' /tmp/b2.out \
	| sed 's/ t=[0-9]*//' | sort -u > /tmp/b2-before.txt
cat /tmp/b2-before.txt

# --- dump with the REAL criu (tree mode) ---
criu dump -t $ROOT -D $IMGS -v4 --shell-job

# --- restore with OUR implementation ---
./userspace/mini-restore/mini-restore -D $IMGS --restore-sibling \
	>> /tmp/b2.out 2>&1 &
sleep 4

# Every task must be back.
for f in /tmp/b2-before.txt; do :; done
FAIL=0
while read -r line; do
	who=$(echo "$line" | awk '{print $1}')
	grep -q "^$who alive" /tmp/b2.out || { echo "MISSING: $who"; FAIL=1; }
done < /tmp/b2-before.txt

# And every task's pid/ppid/pgid/sid must be identical.
grep -E '^(parent|A|B|C|grandchild) alive' /tmp/b2.out \
	| sed 's/ t=[0-9]*//' | sort -u > /tmp/b2-after.txt

if ! diff -u /tmp/b2-before.txt /tmp/b2-after.txt; then
	echo "FAIL: pid/ppid/pgid/sid changed across restore"
	FAIL=1
fi

pkill -9 -f tests/progs/tree-session 2>/dev/null || true
[ "$FAIL" -eq 0 ] || exit 1
echo "B2 OK -- tree with mixed sessions restored identically"
```

**`sed 's/ t=[0-9]*//'` 把 tick 计数去掉再比对**,因为那个数会变,而其他四个数
必须不变。这个细节决定了这个测试是「精确比对」还是「大致看看」。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | 父 + 3 子 的平坦树 | 全部恢复,`ppid` 全对 |
| 2 | 5 层深的链式树 | 全部恢复 |
| 3 | `tree-session` 的混合 session 拓扑 | 四个数全部一致 |
| 4 | 所有进程 `sid` 一致 | `getsid` 输出一致 |
| 5 | 所有进程 `pgid` 一致 | `getpgrp` 输出一致 |
| 6 | 每个进程的内存内容完整 | 无 `CORRUPT` 输出 |
| 7 | 每个进程从各自的 checkpoint 点继续 | 各自的 tick 递增不归零 |
| 8 | 僵尸子进程 | 父进程 `wait()` 拿到相同退出码 |
| 9 | 树里某个 pid 已被占用 | 明确报错,**已创建的 task 被清理** |
| 10 | 组长在别的分支上 | `pgid` 正确,不死锁 |
| 11 | session leader 已死的拓扑 | 明确 `-EOPNOTSUPP`,不死锁 |
| 12 | 50 个进程的树 | 恢复成功,不超时 |
| 13 | 屏障前某个 task 失败 | 其余 task **不永久挂住**(超时生效) |
| 14 | 单进程(1 个任务的「树」) | B1 的所有测试仍然通过(回归) |
| 15 | 恢复后进程间 pipe 仍连通 | 一端写另一端读得到(需 A5 对应的 restore 侧) |

**用例 9 和 13 是 B2 最重要的两个用例**,它们检验的是失败路径:

- 用例 9:部分成功后的清理。已经用 `clone3(set_tid)` 建出来的 task 必须被杀掉,
  否则它们会以「半恢复」的状态留在系统里 —— 这些 task 的地址空间是 mini-restore
  的,pid 却是别人的,极难排查。
- 用例 13:任何一个 task 失败,其余的必须超时退出而不是永久等待屏障。

用例 9 的清理实现:

```c
	/* Partial failure must not leave half-restored tasks behind: they hold
	 * pids that belong to the checkpointed tree and would confuse the next
	 * restore attempt. Kill everything we created, from the root down.
	 */
	if (ret < 0 && root_pid > 0) {
		kill(root_pid, SIGKILL);
		/* The root's own children die with it only if they are still
		 * inside our restore code; signal the whole group as well. */
		kill(-root_pid, SIGKILL);
		waitpid(root_pid, NULL, 0);
	}
```

用例 14 是回归用例:**B2 必须没有破坏 B1。** 一个「为了支持树而改坏了单进程
路径」的实现很常见,因为单进程是树的退化情况,容易在重构时被当成特例删掉。

### 5.3 ZDTM 增量

通过 `restore-shim`(B1 建立的)接上:

```
zdtm/static/pstree
zdtm/static/session00
zdtm/static/session01
zdtm/static/zombie00
zdtm/static/pgrp00
```

**这和 A7 的 ZDTM 列表是同一批测试。** 两条轨在同一批测试上从两个方向收敛,
是这个双轨设计给出的额外好处:同一个测试挂了,可以通过换用哪一侧的真 criu 来
立刻判断问题在哪一侧。

---

## 6. 完成标准

- [ ] 15 个用例通过,含 9、13 两个失败路径用例
- [ ] 共享 scratch 区在第一次 clone **之前** mmap(代码审查确认)
- [ ] 所有跨 task 状态通过 `rst_shared` 访问,`rst_item` 里无跨 task 指针
- [ ] 所有 futex 等待都有超时
- [ ] B1 的 16 个用例全部仍通过
- [ ] `ci/zdtm-restore-allowlist.txt` 增加至少 4 个测试
- [ ] 已知限制更新:根任务 ppid 不可恢复、无 `TASK_HELPER`
