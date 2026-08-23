# 原理 06 —— pid、会话、进程组

> 被引用于:[A4](../steps/A4-threads.md)、[A7](../steps/A7-pstree.md)、
> [B2](../steps/B2-pstree-restore.md)

---

## 1. pid 为什么必须相等

[01-process-anatomy](01-process-anatomy.md) 已经给了结论:**pid 会泄漏进 A 类内存**,
而 A 类是按字节恢复的,里面的 pid 不会被翻译。所以只有一条路:

> **让 pid 去适配进程,而不是让进程去适配 pid。**

这一篇讲这件事在机制上怎么实现,以及由它派生出的会话/进程组问题。

---

## 2. `struct pid` 与 pid namespace

内核里 pid 不是一个整数,是一个对象:

```c
struct pid {
	refcount_t count;
	unsigned int level;			/* 这个 pid 在多少层 ns 里可见 */
	struct hlist_head tasks[PIDTYPE_MAX];	/* 谁在用这个 pid */
	struct upid numbers[1];			/* 变长:每层 ns 里的数值 */
};
```

`numbers[]` 是变长数组,**一个 task 在每一层 pid namespace 里有一个不同的数值。**

```
容器里的进程:
  numbers[0] = 8123   ← 宿主的 init ns 里看到的 pid
  numbers[1] = 42     ← 容器自己的 ns 里看到的 pid
                        level = 1
```

### 三种「pid」

`PIDTYPE_MAX` 说明一个 `struct pid` 可以扮演三种角色:

| 类型 | 含义 |
|---|---|
| `PIDTYPE_PID` | 这是某个 task 的 tid |
| `PIDTYPE_PGID` | 这是某个进程组的 id |
| `PIDTYPE_SID` | 这是某个会话的 id |

**同一个 `struct pid` 可以同时是三者。** 一个会话首进程,它的 pid 既是自己的 tid,
又是进程组 id,又是会话 id。

### dump 侧的直接后果:必须用 `_vnr` 系列

内核里读 pid 有两套函数:

| 函数 | 返回 |
|---|---|
| `task->pid`、`task_pid_nr(t)` | **init namespace** 里的数值 |
| `task_pid_vnr(t)` | **当前 namespace** 里的数值 |

同理有 `task_pgrp_vnr()`、`task_session_vnr()`、`task_tgid_vnr()`。

**dump 必须用 `_vnr` 版本。** 用错的症状很有欺骗性:

```
不在容器里测试     → 两者相等,一切正常
在容器里测试       → 镜像里全是宿主的 pid,restore 出来的进程树关系全错
```

这就是 [A7](../steps/A7-pstree.md) 把「用 `_vnr`」写成硬要求的原因。

---

## 3. `clone3(set_tid)`:指定 pid 创建进程

Linux 5.5 加入了 `clone3()` 的 `set_tid` 字段,**它就是为 C/R 加的。**

```c
	struct _clone_args c_args = {};

	c_args.flags = flags;
	c_args.set_tid = ptr_to_u64(&pid);
	c_args.set_tid_size = 1;
	pid = syscall(__NR_clone3, &c_args, sizeof(c_args));
```

(`criu/criu/clone-noasan.c:75-79`)

### `set_tid_size` 的含义

`set_tid` 是一个**数组**,`set_tid_size` 是它的长度。数组的顺序是**从最内层
namespace 往外**:

```
set_tid[0] = 容器里的 pid       (最内层)
set_tid[1] = 上一层的 pid
set_tid[2] = init ns 里的 pid   (最外层)
```

`set_tid_size = 1` 意思是「**只指定最内层的 pid,外层由内核自由分配**」。

**这一条是容器迁移能成立的全部原因。** 迁移一个容器时,你要保证容器内部看到的
pid 不变,但宿主分给它什么 pid 你不在乎也管不着 —— 目标机器上那个数值可能已经
被别的进程占了。

所以本项目也用 `set_tid_size = 1`。指定多层需要在每一层都有 `CAP_SYS_ADMIN`,
而且几乎总是不必要的。

### 权限

在自己的 pid namespace 里用 `set_tid` 需要该 ns 的 `CAP_SYS_ADMIN`。这就是
CRIU 需要 root(或至少一堆 capability)的原因之一。

---

## 4. 老办法:`ns_last_pid`,以及为什么它不好

在 `clone3(set_tid)` 之前,CRIU 用一个 hack:

```c
	fd = open_proc_rw(PROC_GEN, LAST_PID_PATH);   /* sys/kernel/ns_last_pid */
	len = snprintf(buf, sizeof(buf), "%d", *pid - 1);
	if (write(fd, buf, len) != len) {
```

(`criu/criu/cr-restore.c:1083-1088`,`LAST_PID_PATH` 定义在
`criu/criu/include/util.h:302`)

原理:内核分配 pid 时从 `ns_last_pid + 1` 开始找空位。**写入 `pid - 1`,然后
立刻 fork,大概率就拿到 `pid`。**

三个问题,一个比一个严重:

1. **「大概率」。** 如果 `pid` 已经被占用,内核会跳到下一个空位,你拿到别的 pid。
2. **有竞态。** 写入和 fork 之间,任何别的进程 fork 都会抢走那个 pid。CRIU 为此
   要加一把全局锁(`lock_last_pid()`,`cr-restore.c:1195`)。
3. **跨 namespace 时更麻烦。** 要写另一个 ns 的 `ns_last_pid`,得先有一个进程在
   那个 ns 里 —— CRIU 需要 fork 一个 helper 专门去写:

```c
			if (external_pidns) {
				/*
				 * Restoring into another namespace requires a helper
				 * to write to LAST_PID_PATH. Using clone3() this is
				 * so much easier and simpler. As long as CRIU supports
				 * clone() this is needed.
				 */
				ret = call_in_child_process(set_next_pid, (void *)&pid);
```

(`cr-restore.c:1198-1204`)

CRIU 至今保留这条路径,是为了支持 5.5 之前的内核(`kdat.has_clone3_set_tid` 做
运行时探测)。

**本项目锁定 5.10,所以只实现 `clone3(set_tid)` 一条路径。** 检测不到就明确报错,
不做降级 —— 降级路径无法被测试(在 5.10 上永远走不到),就是死代码。

### 顺带:pid 用完的边界

`clone3(set_tid)` 拿不到指定 pid 时返回 `-EEXIST`。这是一个**必须显式处理并给出
清晰错误**的情况,因为它是 restore 最常见的真实失败原因之一:

```
你 dump 了 pid 1234,没 kill 原进程,然后 restore
→ 1234 还活着 → -EEXIST
```

这就是 [A3](../steps/A3-minimal-dump.md) 的测试脚本用 `--leave-running`
(pid 被占用,验证 dump)而 [B1](../steps/B1-mini-restore.md) 的测试脚本**故意不用**
(pid 空出来,验证 restore)的原因。**同一个测试环境的两个方向,需求恰好相反。**

---

## 5. 会话与进程组:它们其实就是 pid

这是这一篇最有价值的一个认识。

### 定义

| | 是什么 |
|---|---|
| **会话(session)** | 一组进程组的集合。和终端绑定。sid = 首进程的 pid |
| **进程组(pgrp)** | 一组进程,是作业控制的单位。pgid = 首进程的 pid |

**注意「= 首进程的 pid」。** 会话 id 不是一个独立分配的号码,它**就是**会话首进程的
pid。所以:

> **pid 恢复对了,sid 和 pgid 自动就对了 —— 只要「谁是首进程」这个关系对。**

这大幅缩小了问题:不需要「恢复 sid 的值」这种机制(根本没有 `setsid_to(x)` 这样的
系统调用),只需要**让正确的那个进程在正确的时刻调 `setsid()`**。

`setsid()` 返回调用者自己的 pid。既然 pid 已经被 `clone3(set_tid)` 钉住了,
`setsid()` 产生的 sid 必然是正确的值。

**「正确的东西不需要动作,只需要断言。」**

---

## 6. 会话:两遍 fork 的必然性

### 三条不可协商的事实

1. `setsid()` **让调用者成为新会话的首进程**,并把它移出原会话
2. **没有「加入一个已存在会话」的系统调用**
3. 一个进程只能通过 **fork 继承** 进入父进程所在的会话

把三条放一起:

> **一个进程要进入会话 S,唯一的办法是被一个已经在 S 里的进程 fork 出来。**
> 而 `setsid()` 是不可逆的 —— 调过之后,你再也回不到原来的会话。

### 于是就有了这个矛盾

考虑这棵树:

```
P (sid=P)
├── C (sid=P)     ← C 必须在 P 调 setsid() 之前被 fork
└── A (sid=A)     ← A 自己调了 setsid()
```

如果 P 先 `setsid()` 再 fork C,C 会继承 P 的**新**会话 —— 错了,而且**无法补救**。
如果 P 先 fork 完所有孩子再 `setsid()`,那 A 又需要在 P 的旧会话里先被创建出来。

**结论:必须分两遍。**

```
第一遍:fork 所有「必须在我当前会话里出生」的孩子
        ↓
        自己调 setsid()(如果需要)
        ↓
第二遍:fork 其余的孩子
```

CRIU 的实现(`criu/criu/cr-restore.c:1491` 的 `create_children_and_session()`)
正是这个结构,中间那句 `BUG_ON` 是这个设计的自我文档:

```c
		/* Only children from the same session can be restored here */
		BUG_ON(!restore_before_setsid(child));
```

### dump 侧要多存一个字段

「哪些孩子必须在我 setsid 之前出生」这个信息,**内核没有记录**。内核只记录当前的
sid,不记录「你被 fork 的时候你父亲在哪个会话」。

所以 dump 侧要**推导**它。[A7](../steps/A7-pstree.md) 里的做法:

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

这就是 `pstree.proto` 里 `sid` 字段之外还需要额外信息的原因,也是**「dump 侧的
一个字段决定了 restore 侧的整个控制流」的最清楚的例子。**

### restore 侧的断言形态

`restore_sid()`(`cr-restore.c:1373` 起)分两种情况,而且**两种都不是「设置」,
是「验证」**:

```c
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
			pr_err("Requested sid %d doesn't match inherited %d\n", current->sid, sid);
			exit(1);
		}
	}
```

三个要点:

1. **我是首进程** → 调 `setsid()`,然后**检查返回值等于期望的 sid**。因为 pid 已经
   钉住了,这个检查理论上不可能失败 —— 它失败意味着 pid 恢复出了问题。
2. **我不是首进程** → **什么都不做,只 `getsid(0)` 验证继承来的值是对的。**
   这就是「构造式恢复」:关系是 fork 顺序自动带来的,不需要动作。
3. 不匹配就 `exit(1)`。**没有补救路径,因为不存在补救手段。**

---

## 7. 进程组:可以后加入,所以不需要两遍 fork

进程组和会话有一个关键差异,CRIU 的注释把它说得很清楚
(`cr-restore.c:1396` 起):

```c
	/*
	 * Unlike sessions, process groups (a.k.a. pgids) can be joined
	 * by any task, provided the task with pid == pgid (group leader)
	 * exists. Thus, in order to restore pgid we must make sure that
	 * group leader was born and created the group, then join one.
	 *
	 * We do this _before_ finishing the forking stage to make sure
	 * helpers are still with us.
	 */
```

**`setpgid()` 可以加入一个已存在的进程组**(条件是组长还活着,且在同一会话里)。
所以不需要靠 fork 顺序来安排 —— 但**仍然有顺序要求**:组长必须先建好组。

区别在于「怎么满足这个顺序」:

| | 会话 | 进程组 |
|---|---|---|
| 能否后加入 | **否** | 是(`setpgid`) |
| 顺序怎么保证 | **靠 fork 的先后**(结构性) | **靠等待**(同步性) |
| 手段 | 两遍 fork | futex 等组长就位 |

CRIU 用 futex:

```c
		leader = rsti(current)->pgrp_leader;
		if (leader) {
			BUG_ON(my_pgid != vpid(leader));
			futex_wait_until(&rsti(leader)->pgrp_set, 1);
		}
	}

	pr_info("\twill call setpgid, mine pgid is %d\n", pgid);
	if (setpgid(0, my_pgid) != 0) {
```

组长自己在 `setpgid` 成功后唤醒所有等待者:

```c
	if (my_pgid == vpid(current))
		futex_set_and_wake(&rsti(current)->pgrp_set, 1);
```

**「结构性顺序」和「同步性顺序」的区别值得记住**,它在 restore 里反复出现:
前者错了就完全不工作(而且往往不可补救),后者错了表现为死锁。
详见 [09-restore-ordering](09-restore-ordering.md)。

### 一个实践要求:所有 futex 等待都要有超时

CRIU 的 `futex_wait_until()` 会无限等。对本项目来说这是不可接受的:**开发期一定会
写出「等一个永远不会到来的信号」的 bug**,而无限等的症状是整个测试挂住,还得
去猜是哪个进程在等谁。

带超时的版本在超时时能打印「我是谁、我在等谁、等了多久」,这把一个需要 gdb 的
问题变成一行日志。这是 [B2](../steps/B2-pstree-restore.md) 的一条硬要求。

---

## 8. 线程:pid 的另一面

`clone3(set_tid)` 同样用于恢复线程 —— 线程就是「tid 不同、tgid 相同」的 task。

关键约束:**`CLONE_THREAD` 要求 `set_tid` 里指定的是 tid**,而 tgid 由被加入的
线程组决定。所以线程的恢复顺序是:

```
1. 先用 set_tid = 主线程 tid 创建 group leader
2. 再用 set_tid = 各线程 tid + CLONE_THREAD 加入这个组
```

这个顺序是**结构性**的:`CLONE_THREAD` 必须指定要加入哪个线程组,那个组得先存在。

### 为什么线程的 tid 也必须精确

`pthread_t` 在 glibc 里就是线程栈的地址,而 `pthread_join` 等操作最终要用 tid 去
`tgkill`。更隐蔽的是 **robust futex**:`pthread_mutex` 的所有者字段里存的是 tid。

```
线程 T1 (tid=1235) 持有一个 mutex → mutex 的 owner 字段 = 1235
恢复后 T1 的 tid 变成 1240
T1 调 pthread_mutex_unlock() → 内核检查 owner != 当前 tid → EPERM
```

**进程死锁在自己持有的锁上。** 而且这个 bug 只在「恢复时恰好持有锁」时出现,
测试很难覆盖到。

这就是 [A4](../steps/A4-threads.md) 强调「tid 全等」不是锦上添花的原因。

---

## 9. 一张对照表

| | 值从哪来 | restore 手段 | 顺序类型 | 能否事后修正 |
|---|---|---|---|---|
| **pid / tid** | 镜像 | `clone3(set_tid)` | —— | 否 |
| **tgid** | 由线程组决定 | `CLONE_THREAD` | 结构性 | 否 |
| **父子关系** | 镜像 | 递归 fork | 结构性 | 否(除 `CLONE_PARENT`) |
| **sid** | = 首进程 pid | `setsid()` + 两遍 fork | **结构性** | **否** |
| **pgid** | = 组长 pid | `setpgid()` + futex 等待 | 同步性 | 是 |

**最后一列是这张表的重点:** 只有 pgid 可以事后修正。其余每一项,创建那一刻错了
就永久错了 —— 这决定了它们必须在 fork 时就正确,也决定了 dump 侧必须把足够的
信息(包括 `born_sid` 这种推导出来的信息)一次性提供齐。

---

## 10. 延伸阅读

- `kernel/pid.c` —— `alloc_pid()`,**`set_tid` 就在这里被消费**,读它能看清
  「指定 pid」到底发生了什么
- `include/linux/pid.h` —— `struct pid`、`struct upid`、`PIDTYPE_*`
- `kernel/sys.c` 的 `sys_setsid()` / `sys_setpgid()` —— 权限检查和约束条件,
  比 man page 精确
- `criu/criu/cr-restore.c:1373`(`restore_sid`)、`:1396`(`restore_pgid`)、
  `:1491`(`create_children_and_session`)
- `criu/criu/clone-noasan.c:48-84` —— `clone3_with_pid_noasan()`
- `man 2 clone3` 的 `set_tid` / `set_tid_size` 段落
- `man 7 credentials` 的会话与进程组部分
- [01-process-anatomy](01-process-anatomy.md) —— pid 泄漏的七条途径
- [09-restore-ordering](09-restore-ordering.md) —— 结构性 vs 同步性顺序的完整讨论
