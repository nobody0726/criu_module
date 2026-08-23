# X1 —— 进程启动时间:唯一只有内核能做的事

**工期:** 1 周 · **前置:** A3 · **产出:** 一个 CRIU 无法实现的内核接口

> 相关原理:[08-kernel-module-limits](../principles/08-kernel-module-limits.md)

---

## 1. 设计思路

### 为什么单独有这一步

整个计划里,A 轨证明的是「内核模块能做得更干净」(不用 parasite、不用 ptrace、
不用解析 `/proc` 文本),B 轨证明的是「有些事只能在用户态做」。

**X1 是唯一一个证明「有些事只有内核能做」的步骤。** 它的价值不在于功能重要,
而在于它是这个项目技术价值的一个不可替代的证明点。

### 缺口在哪

`criu/images/core.proto:64`:

```protobuf
	// Reserved for container relative start time
	//optional uint64		start_time	= 19;
```

**字段被注释掉了。** CRIU 预留了位置,但没有实现。原因不是它不想实现,而是
**内核没有提供写接口**。

`task->start_time` 和 `task->start_boottime` 是内核在 `copy_process()` 里设的,
之后再也不改。没有任何系统调用能修改它。用户态程序无论有多少权限,都无法让一个
进程的启动时间变成过去的某个值。

### 这个缺口的可观测后果

| 观察方式 | 恢复后的值 | 应该是 |
|---|---|---|
| `ps -o lstart` | restore 的时刻 | 原始启动时刻 |
| `ps -o etime`(运行时长) | 从 restore 算起 | 从原始启动算起 |
| `/proc/PID/stat` 第 22 字段 | restore 时的 jiffies | 原始值 |
| `times()` 系统调用 | 部分受影响 | —— |

对大多数程序无所谓。但有几类程序会被影响:

- **监控/计费系统** —— 按进程运行时长计费或告警
- **看门狗** —— 「进程运行超过 N 小时就重启」的逻辑会被重置
- **审计** —— 进程启动时间是取证时的一个关键时间锚点
- **`pgrep -o`(最老的进程)** 之类的工具会给出错误答案

**这不是致命问题,但它是一个真实的、可观测的、CRIU 无法修复的语义偏差。**

### X1 的范围要非常小

X1 不是「实现完整的 start_time 恢复」。它是:

1. **一个内核接口**,让特权用户态代码能设置某个 task 的 `start_time`
2. **dump 侧**把值存进镜像(用那个被注释掉的字段 19)
3. **restore 侧**(B 轨或一个独立工具)调用这个接口

**范围小是刻意的。** 它是一个演示/证明性质的步骤,不是一个产品特性。

---

## 2. CRIU 是怎么做的(参照)

### 2.1 CRIU 什么都没做,这就是参照

这是唯一一个「参照 CRIU 的实现」等于「参照 CRIU 的不实现」的步骤。值得看的是
CRIU **为这个缺口做的准备**:

- `core.proto:64` 预留了字段号 19。**字段号被预留意味着它不会被别的东西占用**,
  我们启用它不会造成格式冲突
- 注释里写的是「Reserved for **container relative** start time」—— 
  CRIU 设想的语义是「相对于容器启动时刻的时间」,不是绝对墙上时钟

第二点很重要,它影响我们该存什么值。

### 2.2 该存绝对值还是相对值

| 方案 | 优点 | 缺点 |
|---|---|---|
| 绝对 `CLOCK_REALTIME` 时刻 | 直观,`ps -o lstart` 直接对 | 跨机器迁移时如果两机时钟不同步就错 |
| 相对于容器/命名空间启动的偏移 | 跨机器语义正确 | 需要一个「容器启动时刻」的锚点 |
| 相对于 dump 时刻的偏移(即「已运行多久」) | 只需一个数,无锚点 | restore 后运行时长连续,但 `lstart` 变成 restore 时刻减去时长 |

**X1 选第三个:存「已运行多久」。**

理由:它是三者里唯一不需要任何外部锚点的,而且它恢复的正是最有用的那个量 —— 
`etime`(运行时长)。`lstart` 会被算成 `restore_time - etime`,在同一台机器上
dump-restore 的情况下这个值就是原始启动时刻。跨机器时它取决于两机时钟同步程度,
但这个误差是**有界且可解释的**,不像方案一那样可能出现负的运行时长。

这和 A6 里定时器「存剩余时间而非绝对到期时刻」是**同一个设计取向**:
存相对量,让 checkpoint/restore 之间的时间对进程不可见。

**A6 和 X1 在这一点上保持一致,是这套设计的内在一致性。** 如果 X1 存绝对时间
而 A6 存相对时间,那么恢复后一个进程会「觉得」自己刚启动没多久(定时器视角)
但 `ps` 说它已经跑了三天(start_time 视角),两个视角互相矛盾。

---

## 3. 文件结构

**Create:**
- `kernel_module/extra/set_start_time.c` —— 新接口的实现
- `userspace/set-start-time/main.c` —— 调用它的小工具
- `tests/x1-start-time.sh`

**Modify:**
- `kernel_module/checkpoint/dump_core.c` —— 填字段 19
- `kernel_module/images/core.proto` —— **本地副本**里取消注释字段 19

**Interfaces produced:**

```c
/* Set a task's start time from its elapsed run time. There is no syscall that
 * can do this: task->start_time is written once in copy_process() and never
 * again, which is exactly why criu leaves core.proto field 19 commented out.
 *
 * Requires CAP_SYS_ADMIN. The target must be stopped (we do not take any lock
 * that would make a concurrent read safe, and there is no reader that expects
 * this value to change).
 *
 * elapsed_ns: how long the task had been running at checkpoint time.
 */
int criu_set_start_time(pid_t vpid, u64 elapsed_ns);
```

debugfs 接口:

```
/sys/kernel/debug/criu/set_start_time     write: "<pid> <elapsed_ns>"
```

**注释里那句「there is no syscall that can do this」是这个接口存在的全部理由,
所以它必须写在头文件里。** 否则后来的人会问「为什么不用系统调用」。

---

## 4. 关键实现要点

### 4.1 要改的是两个字段,不是一个

5.10 的 `task_struct` 里:

```c
	u64				start_time;		/* Monotonic time in nsecs */
	u64				start_boottime;		/* Boot based time in nsecs */
```

`start_time` 基于 `CLOCK_MONOTONIC`(不含挂起时间),`start_boottime` 基于
`CLOCK_BOOTTIME`(含挂起时间)。`/proc/PID/stat` 用前者,某些接口用后者。

**两个都要改,否则不同的观察途径会给出矛盾的答案。** 这是一个「只改一个也能通过
粗糙测试」的坑 —— `ps -o etime` 只看一个。

```c
	/* Both clocks must move together: /proc/PID/stat reads start_time while
	 * some interfaces read start_boottime, and leaving them inconsistent
	 * makes the process's age depend on which tool you ask.
	 */
	now_mono = ktime_get_ns();
	now_boot = ktime_get_boottime_ns();

	task->start_time     = now_mono - elapsed_ns;
	task->start_boottime = now_boot - elapsed_ns;
```

### 4.2 下溢检查

如果 `elapsed_ns` 大于系统已经运行的时间(比如从一台开机很久的机器 dump,
restore 到一台刚开机的机器),`now_mono - elapsed_ns` 会下溢成一个巨大的正数。

```c
	/* A task cannot have started before the system booted. Clamp instead of
	 * underflowing: a wrong-but-sane value beats a 500-year-old process,
	 * which breaks every tool that formats it.
	 */
	if (elapsed_ns > now_mono) {
		pr_warn("criu: elapsed %llu ns exceeds uptime %llu ns, clamping\n",
			elapsed_ns, now_mono);
		elapsed_ns = now_mono;
	}
```

**这个检查不是防御性编程的冗余,它是一个真实会发生的情况**,而且不检查的后果
(`ps` 显示一个 584 年前启动的进程)会让所有依赖时间格式化的工具出问题。

### 4.3 为什么这个接口在安全上是可接受的

一个能修改任意进程 `start_time` 的接口,值得想清楚它的滥用面:

| 顾虑 | 评估 |
|---|---|
| 能否用来提权 | 否。`start_time` 不参与任何权限判断 |
| 能否用来隐藏进程 | 部分能 —— 能干扰 `pgrep -o`、`ps` 排序,以及基于运行时长的审计 |
| 能否造成内核崩溃 | 否,只是两个 `u64` 字段。但下溢会让时间计算给出荒谬结果 |
| 谁能调用 | 要求 `CAP_SYS_ADMIN`,和 `criu dump` 本身同一级别 |

**结论:滥用面局限在「干扰审计工具的时间视图」。** 这不是零风险,而且值得明确
写出来:一个攻击者如果已经有 `CAP_SYS_ADMIN`,他有远比这个更直接的手段。

但**这正是它不适合进上游内核的理由之一**,也是它作为一个 out-of-tree 模块接口
存在的合理位置。写进限制文档,不要假装它没有代价。

### 4.4 是否需要加锁

`start_time` 在 `copy_process()` 之后没有任何写者,读者都是 `/proc` 之类的
无锁读取。所以:

```c
	/* No lock: this field has no other writer after copy_process(), and all
	 * readers (procfs, getrusage paths) do unlocked reads of a u64 that is
	 * naturally aligned. A concurrent reader sees either the old or the new
	 * value, both of which are self-consistent.
	 */
```

**但两个字段之间存在一个窗口**:改完 `start_time` 还没改 `start_boottime` 时,
一个并发读者可能看到不一致的组合。规避:要求目标任务已停止(restore 时它必然
是停止的),并在接口文档里写明这个前提。

### 4.5 本地 proto 副本

启用字段 19 需要修改 proto。**不要改 `criu/images/core.proto`** —— 那是参照实现,
改了它会让所有「与 CRIU 严格一致」的对比失去意义。

做法:`kernel_module/images/core.proto` 是一份本地副本,只有这一处差异,
并且差异要被显式记录:

```protobuf
	// ENABLED BY X1. Upstream criu leaves this commented out because there is
	// no userspace way to write task->start_time; our module provides one via
	// /sys/kernel/debug/criu/set_start_time. Field number 19 is the one
	// upstream reserved, so images stay compatible: a criu that ignores
	// field 19 restores everything else normally.
	optional uint64			start_time	= 19;
```

**关键性质:启用一个 optional 字段不破坏向后兼容。** 真 criu 读到字段 19 会
忽略它(protobuf 的未知字段被跳过),其余字段照常恢复。所以 A3 建立的
「我们的镜像真 criu 认得」这个门禁**不会因为 X1 而失效**。

这一点必须验证,是测试用例 7。

---

## 5. 如何测试

### 5.1 验收脚本

```sh
#!/bin/sh
# X1: prove we can restore a process's start time -- the one thing criu cannot
# do at all, because the kernel has no write path for task->start_time.
set -e

./tests/progs/minimal > /tmp/x1.out 2>&1 &
PID=$!
# Let it accumulate a clearly measurable age.
sleep 30

# /proc/PID/stat field 22 is starttime, in clock ticks since boot.
BEFORE_START=$(awk '{print $22}' /proc/$PID/stat)
BEFORE_ETIME=$(ps -o etimes= -p $PID | tr -d ' ')
echo "before: starttime=$BEFORE_START etimes=$BEFORE_ETIME"
[ "$BEFORE_ETIME" -ge 28 ] || { echo "FAIL: setup, process too young"; exit 1; }

IMGS=/tmp/x1-imgs
rm -rf $IMGS && mkdir -p $IMGS

insmod kernel_module/criu_kernel.ko
echo "$PID $IMGS" > /sys/kernel/debug/criu/dump

# Field 19 must actually be in the image.
crit decode -i $IMGS/core-$PID.img --pretty | grep -q '"start_time"' || {
	echo "FAIL: start_time not in image"; exit 1; }

kill -9 $PID
sleep 1

criu restore -D $IMGS --restore-detached >> /tmp/x1.out 2>&1
sleep 2

# Without X1 the restored process looks brand new.
MID_ETIME=$(ps -o etimes= -p $PID | tr -d ' ')
echo "after restore, before fixup: etimes=$MID_ETIME"

# Apply the fixup: elapsed at checkpoint was BEFORE_ETIME seconds.
ELAPSED_NS=$(( BEFORE_ETIME * 1000000000 ))
echo "$PID $ELAPSED_NS" > /sys/kernel/debug/criu/set_start_time

AFTER_ETIME=$(ps -o etimes= -p $PID | tr -d ' ')
echo "after fixup: etimes=$AFTER_ETIME"

# It must now report an age close to the original, not near zero.
[ "$AFTER_ETIME" -ge $(( BEFORE_ETIME - 3 )) ] || {
	echo "FAIL: etimes $AFTER_ETIME, expected >= $(( BEFORE_ETIME - 3 ))"
	exit 1; }

# And lstart must be a sane, formattable date.
LSTART=$(ps -o lstart= -p $PID)
echo "lstart=$LSTART"
case "$LSTART" in
	*19[0-9][0-9]*|*2[1-9][0-9][0-9]*|"") echo "FAIL: absurd lstart"; exit 1 ;;
esac

kill -9 $PID 2>/dev/null || true
rmmod criu_kernel
echo "X1 OK -- start time restored, which criu alone cannot do"
```

**这个脚本刻意在 fixup 前后各测一次 `etimes`。** `MID_ETIME`(接近 0)和
`AFTER_ETIME`(接近 30)的对比,就是 X1 价值的直接演示 —— 它同时证明了
「问题真实存在」和「我们的接口解决了它」。只测后者的话,读者无法判断问题原本
有多严重。

### 5.2 测试用例

| # | 用例 | 判定 |
|---|---|---|
| 1 | dump 侧字段 19 被填充 | `crit decode` 里有 `start_time` |
| 2 | fixup 后 `ps -o etimes` 接近原值 | 差值 ≤ 3 秒 |
| 3 | fixup 后 `ps -o lstart` 是合理日期 | 不是 1970 也不是 2500 |
| 4 | `/proc/PID/stat` 第 22 字段一致 | 与原值接近 |
| 5 | `start_time` 和 `start_boottime` 都被改 | 两种观察途径不矛盾 |
| 6 | `elapsed_ns` > 系统 uptime | 被 clamp,**不下溢** |
| 7 | **启用字段 19 后真 criu 仍能 restore** | A3 的门禁测试仍然绿 |
| 8 | 非特权用户调用接口 | `-EPERM` |
| 9 | pid 不存在 | `-ESRCH` |
| 10 | `elapsed_ns = 0` | `start_time` 变成「刚启动」,不报错 |
| 11 | 多线程进程 | 所有线程的 `start_time` 处理一致 |
| 12 | 对同一进程调用两次 | 第二次生效,不累积 |

**用例 7 是这一步的门禁。** 它验证 X1 没有破坏 A3 建立的二进制兼容性。判定
方法就是重跑 `tests/cross-restore.sh`,必须仍然通过。

用例 6 的构造:传一个大于 uptime 的 `elapsed_ns`。

```sh
# Underflow guard: a process cannot predate the boot. Ask for an age larger
# than the system uptime and require a clamp, not a 584-year-old process.
UPTIME_NS=$(( $(awk '{print int($1)}' /proc/uptime) * 1000000000 ))
echo "$PID $(( UPTIME_NS * 2 ))" > /sys/kernel/debug/criu/set_start_time
dmesg | tail -5 | grep -q 'clamping' || { echo "FAIL: no clamp warning"; exit 1; }
ETIME=$(ps -o etimes= -p $PID | tr -d ' ')
[ "$ETIME" -lt $(( UPTIME_NS / 1000000000 + 5 )) ] || {
	echo "FAIL: etimes $ETIME exceeds uptime"; exit 1; }
```

用例 11 值得注意:线程的 `start_time` 各自独立(每个 task 一份),而且它们的值
**本来就不同**(线程是在不同时刻创建的)。所以每个线程要各自 fixup,不能用主
线程的值覆盖所有线程。**这是一个「看起来该统一、实际该分开」的字段。**

### 5.3 ZDTM

ZDTM 里没有测 `start_time` 的测试(因为 CRIU 不支持它)。**X1 是唯一一个
需要自建测试、无法从 ZDTM 白拿的步骤。**

这本身就说明了它的性质:它超出了 CRIU 的能力边界,所以也超出了 CRIU 测试集的
覆盖范围。

---

## 6. 完成标准

- [ ] 12 个用例通过,含 6、7 两个关键用例
- [ ] `start_time` 和 `start_boottime` 都被修改
- [ ] 下溢 clamp 已实现并有 `pr_warn`
- [ ] 本地 `core.proto` 的差异有注释说明,且只有这一处差异
- [ ] `tests/cross-restore.sh`(A3 门禁)仍然通过
- [ ] 安全评估(4.3 节)写进 `docs/principles/08-kernel-module-limits.md`
- [ ] 在 `03-Iteration-Plan.md` 里把 X1 标为「已证明:内核模块的不可替代能力」
