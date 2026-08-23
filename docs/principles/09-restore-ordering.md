# 原理 09 —— restore 的顺序,以及顺序为什么是 restore 的全部难点

> 被引用于:[A7](../steps/A7-pstree.md)、[B1](../steps/B1-mini-restore.md)、
> [B2](../steps/B2-pstree-restore.md)

---

## 1. 两种依赖:dump 和 restore 的根本不对称

这是整个迭代计划的设计依据,值得先立住。

| | **dump** | **restore** |
|---|---|---|
| 本质 | 纯读 | 一串状态变更 |
| 依赖类型 | **数据依赖** | **顺序依赖** |
| 能否重排 | 能(除「冻结先行」) | 不能 |
| 能否并行 | 能 | 基本不能 |
| 失败了怎样 | 重试即可,目标进程不受影响 | **可能留下一堆半成品进程** |
| 能否验证中间态 | 能,随时 | 过了某个点就不能了 |

**dump 的「依赖」只是「A 的输出是 B 的输入」**,比如要先知道有哪些 VMA 才能读页。
这类依赖是数据流的,可以缓存、可以重算、可以换顺序。

**restore 的「依赖」是「A 必须发生在 B 之前,否则 B 永远不可能正确」。**
这类依赖里有一部分**根本没有补救手段** —— 不是「难以修复」,是不存在能修复它的
系统调用。

**所以 dump 是一个数据采集问题,restore 是一个状态机问题。** 这也是为什么本项目
的 A 轨(dump)能拆成 8 个可独立测试的步骤,而 B 轨只有 2 步就已经很紧张。

---

## 2. 顺序依赖的三种类型

不是所有「顺序」都同样严格。区分它们能极大简化设计。

### 类型一:结构性顺序(不可补救)

**关系在创建的那一刻确定,之后没有任何系统调用能改变它。**

| 关系 | 由什么决定 | 有补救手段吗 |
|---|---|---|
| 父子关系 | 谁 fork 了谁 | 否(`CLONE_PARENT` 只能在 fork 时选) |
| 线程组归属 | `CLONE_THREAD` 时指定 | 否 |
| pid / tid | `clone3(set_tid)` | 否 |
| **会话归属** | 从父进程继承 | **否 —— 不存在 join-session 系统调用** |
| 地址空间共享 | `CLONE_VM` | 否 |
| fd 表共享 | `CLONE_FILES` | 否 |

**这一类必须靠「按正确的顺序创建」来满足。** 它们是 restore 流程形状的决定因素:
递归 fork 的结构、两遍 fork 的必要性,全部来自这一列。

### 类型二:同步性顺序(可补救,但需要等待)

**动作可以后做,但要等前置条件就绪。**

| 关系 | 手段 | 需要等什么 |
|---|---|---|
| 进程组归属 | `setpgid()` | 组长已创建组 |
| 打开一个 pipe 的一端 | `SCM_RIGHTS` 接收 | 创建者已建好并发送 |
| 共享内存映射 | `mmap` 同一对象 | 第一个进程已建好对象 |

**这一类靠等待(futex)满足,不需要安排创建顺序。** 它们的错误症状是**死锁**,
而不是错误的状态 —— 这是好事:死锁明显,错误状态隐蔽。

### 类型三:单向阀门(过了就回不去)

**做完之后,你失去了继续做别的事的能力。**

| 动作 | 失去了什么 |
|---|---|
| **替换地址空间** | 自己的代码和栈 —— **失去了执行任何代码的能力** |
| `rt_sigreturn` | 控制权,不返回 |
| 降低 creds | 重新提权的能力 |
| `setsid()` | 回到原会话的能力 |

**这一类决定了「什么必须最后做」。** 而且它们决定了错误处理的边界:阀门之前的
失败可以清理并报告,阀门之后的失败**连报错都做不到**。

---

## 3. 唯一的强 A/B 约束

[01-process-anatomy](01-process-anatomy.md) 给过结论,这里给出它的全部推论:

> **所有 B 类恢复必须在 A 类恢复(地址空间替换)之前完成。**

理由:做替换的那段代码住在地址空间里,换完它自己就不存在了。

于是 restore 的形状**必然**是:

```
┌─ 所有 B 类恢复 ────────────────────────┐
│  fd、creds、fs、信号、定时器、树结构    │  ← 还活着,能报错,能清理
│  顺序在这里面还有讲究(见第 4 节)      │
└────────────────────────────────────────┘
                  ↓
        ═══ 单向阀门:替换地址空间 ═══      ← 悬崖
                  ↓
             rt_sigreturn                  ← 不返回
```

**「B 类全部先做」这一条,让 restore 有了一个可以充分验证的阶段。**
阀门之前,你可以检查一切、可以放弃、可以打印诊断。这是
[B1](../steps/B1-mini-restore.md) 把「所有可检测的失败都必须在 `clone3` 之前
发现」写成硬要求的原因 —— 它把验证窗口推到了最前面。

---

## 4. B 类内部的顺序:CRIU 的八个阶段

B 类之间也有顺序。CRIU 把它编码成一个显式的状态机,
`criu/criu/include/restorer.h:282-339`:

```c
	CR_STATE_FAIL = -1,
	CR_STATE_ROOT_TASK = 0,
	CR_STATE_PREPARE_NAMESPACES,
	CR_STATE_FORKING,
	CR_STATE_PRE_RESTORER,
	CR_STATE_RESTORE,
	CR_STATE_RESTORE_SIGCHLD,
	CR_STATE_RESTORE_CREDS,
	CR_STATE_COMPLETE
```

每个阶段由 criu 主进程发起,**所有参与的 task 都确认完成后**才进下一个 —— 这是一个
分布式屏障(`restore_finish_stage()` 宏就是它的实现)。

### 为什么需要屏障

因为顺序约束是**跨进程**的。「组长必须先建好组」是进程 A 和进程 B 之间的约束,
不是单个进程内部的先后。**restore 的顺序问题本质上是一个多进程同步问题**,
这是它比 dump 难得多的另一个原因。

### 三个阶段的理由值得单独看

**`CR_STATE_FORKING` 的特殊性:**

```
	 * This stage is a little bit special. Normally all stages
	 * are controlled by criu process, but when this stage
	 * starts criu process starts waiting for the tasks to
	 * finish it, but by the time it gets woken up the stage
	 * finished is CR_STATE_RESTORE. The forking stage is
	 * barrier-ed by the root task
```

fork 阶段由 **root task 而不是 criu 主进程**做屏障。理由是性能(减少上下文切换),
但它揭示了一件事:**fork 出来的树天然形成一个层级,root task 是这个层级的天然
协调者。**

**`CR_STATE_RESTORE_CREDS` 为什么是最后一个:**

```
	 * For security reason processes can be resumed only when all
	 * credentials are restored. Otherwise someone can attach to a
	 * process, which are not restored credentials yet and execute
	 * some code.
	 * Seccomp needs to be restored after creds.
	 * Dumpable and pdeath signal are restored after seccomp.
```

**这是一个安全约束,不是技术约束。** restore 期间进程以高权限运行(要 root 才能
`clone3(set_tid)`)。如果在降权之前就让进程可被调试,攻击者能 attach 上去以高权限
执行代码。

而且它内部还有顺序:**creds → seccomp → dumpable/pdeath_sig**。
seccomp 在 creds 之后,因为 seccomp 一旦装上就限制了后续能调的系统调用 —— 
**又一个单向阀门。**

**这一条对本项目的直接要求:** creds 恢复必须是 B 类里的最后一步,而且
`dumpable` 的恢复必须在它之后。顺序反了不会有功能症状,只有一个安全窗口 —— 
**没有测试能发现它,只能靠把理由写下来。**

---

## 5. 本项目的顺序清单

把三种类型套到本项目的范围上,得到一张完整的约束表:

| # | 动作 | 必须在…之后 | 类型 | 错了的症状 |
|---|---|---|---|---|
| 1 | 读镜像、校验 | —— | —— | —— |
| 2 | 校验一切可校验的 | 1 | —— | (故意放在最前) |
| 3 | 建共享 scratch(MAP_SHARED) | 2,**且在任何 clone 之前** | 结构性 | **死锁** |
| 4 | `clone3(set_tid)` root | 3 | 结构性 | `-EEXIST` |
| 5 | 第一遍 fork(`born_sid` 的孩子) | 4 | **结构性** | sid 永久错 |
| 6 | `setsid()` | 5 | **单向阀门** | 同上 |
| 7 | 第二遍 fork(其余孩子) | 6 | 结构性 | sid 永久错 |
| 8 | 线程(`CLONE_THREAD`) | 4(所属 leader 已建) | 结构性 | tid 错 |
| 9 | `setpgid()` | 组长已建组 | 同步性 | 死锁或 `EPERM` |
| 10 | 恢复 fd(`open` + `dup2`) | 4 | 数据 | `EBADF` / 位置错 |
| 11 | pipe 两端配对(`SCM_RIGHTS`) | 创建者已建 | 同步性 | 死锁 |
| 12 | 恢复 fs(cwd/root) | 4 | 数据 | 相对路径解析错 |
| 13 | 恢复信号处理表、掩码 | 4 | 数据 | —— |
| 14 | premap VMA + 填内容 | 10-13 | 数据 | —— |
| 15 | **恢复 creds** | 所有需要权限的动作之后 | **单向阀门** | 安全窗口 |
| 16 | 恢复 seccomp | 15 | 单向阀门 | —— |
| 17 | 恢复 dumpable / pdeath_sig | 16 | —— | —— |
| 18 | 等所有 task 就绪 | 17 | 同步性 | 死锁 |
| 19 | **搬到 bootstrap,unmap 旧空间,mremap** | 18 | **单向阀门** | **不可诊断** |
| 20 | `rt_sigreturn` | 19 | 单向阀门 | 不返回 |

**第 18 和第 19 之间是整个 restore 的悬崖。**

第 19 步之后,你失去了:执行任意代码的能力、打印日志的能力、访问镜像文件的能力
(fd 还在,但没有代码去读)、以及放弃并清理的能力。

**所以第 18 步的「等所有 task 就绪」不是礼貌,是必需的**:如果 task A 已经跳下悬崖
而 task B 还在恢复 fd,B 失败了就没人能通知 A 停下 —— A 已经是目标进程了,
它会带着一个不完整的兄弟继续运行。

---

## 6. 失败清理:一个必须提前设计的问题

restore 失败在**第 4 步之后、第 19 步之前**,是最需要认真对待的情况:此时已经有
若干个进程被创建出来了。

### 为什么它比看起来难

1. 这些进程的 pid 是**目标进程的 pid**。留着它们,下一次 restore 会 `-EEXIST`
2. 它们可能已经 `setsid()`(离开了你的会话),`kill(-pgid)` 打不到
3. 它们可能正阻塞在某个 futex 上等一个永远不会到来的信号
4. 它们可能已经恢复了 creds(降权了),你的清理逻辑可能反而没权限动它们

### 结论

**清理必须用记录下来的 pid 逐个 `kill()`,不能靠进程组。** 而且要
`waitpid()` 确认它们真的死了 —— 因为下一次 restore 需要那些 pid 空出来。

这就是 [B2](../steps/B2-pstree-restore.md) 把「部分失败后的清理」写成一个独立
测试用例(而不是一句「注意清理」)的原因,也是那个用例要同时用
`kill(root_pid)` 和 `kill(-root_pid)` 两种方式验证的原因 —— **前者验证清理有效,
后者验证「靠进程组清理」确实不够。**

### 而 dump 侧没有这个问题

dump 失败只需要:解冻、把目标移回原 cgroup、删掉半成品镜像文件。**目标进程完全
不知道发生过什么。**

这个不对称是整个迭代计划把 A 轨排在前面的原因之一:**A 轨的每一步都可以放心地
反复试错,B 轨的每一次失败都要清理现场。**

---

## 7. 三个反复出现的判据

这一篇的内容可以压缩成三句可操作的话:

**一、「正确的东西不需要动作,只需要断言。」**

pid 钉住了,sid 就自动对了 —— `restore_sid()` 里非首进程的分支**什么都不做,
只 `getsid(0)` 验证**。找到「哪些正确性是自动的」,能删掉大量代码,也能把剩下的
代码里真正的风险暴露出来。

**二、「不可补救的关系必须在创建时正确。」**

判断一个约束属于哪一类,方法是问:**有没有一个系统调用能事后修正它?**
没有 → 它决定创建顺序。有 → 它只需要一个等待。

**三、「阀门之前尽可能多地验证。」**

因为阀门之后连报错都做不到。这解释了 B1 为什么把
`assert(ctx->core && ctx->mm && ctx->pagemaps && ctx->pages_fd >= 0)` 放在
`clone3` 之前 —— **在还有诊断能力的时候用尽诊断能力。**

---

## 8. 延伸阅读

- `criu/criu/include/restorer.h:275-339` —— 八个阶段和每个阶段的理由注释。
  **这段注释是 CRIU 里信息密度最高的地方之一,值得完整读**
- `criu/criu/cr-restore.c:641` —— `restore_one_alive_task()`,单个 task 的恢复顺序
- `criu/criu/cr-restore.c:1491` —— `create_children_and_session()`,两遍 fork
- `criu/criu/pie/restorer.c:2264` 起 —— 悬崖那一段的实际代码
- `kernel/fork.c` 的 `copy_process()` —— **所有「结构性顺序」的根源都在这个函数里**:
  哪些关系在这里被一次性确定,就是哪些关系不可补救
- [01-process-anatomy](01-process-anatomy.md) —— A/B 二分
- [06-pid-and-session](06-pid-and-session.md) —— 两遍 fork 的完整推导
- [05-registers-and-sigframe](05-registers-and-sigframe.md) —— 最后一跳
- [08-kernel-module-limits](08-kernel-module-limits.md) —— 为什么 restore 在用户态
