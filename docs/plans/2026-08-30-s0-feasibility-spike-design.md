# S0 Feasibility Spike Design

**Date:** 2026-08-30

**Status:** Approved

## Goal

验证 Linux 5.10.29 ARM64 外置内核模块能够安全使用哪些 CRIU dump 所需的
task、mm、VMA 和用户页访问原语，并用实验事实确认哪些 restore 相关内核路径
因未导出或语义副作用而不可采用。

S0 的产出是可重复的实验结论和证据，不是 A1 的实现基础。实验代码是一次性
spike，完成后删除或明确隔离，不能在其上继续堆叠后续功能。

## Scope And Constraints

- 宿主是 Apple Silicon M4 macOS，只负责编辑、Git 和启动 Lima。
- Lima guest 负责构建模块、构建静态 ARM64 测试程序和运行编排脚本。
- QEMU disposable guest 是唯一执行 `insmod`、`rmmod` 和内核接口调用的环境。
- 目标内核是 Linux 5.10.29 ARM64。
- 所有实验使用同一套已构建内核配置，至少包括：
  `CONFIG_MODULES=y`、`CONFIG_DEBUG_VM=y`、`CONFIG_DEBUG_VM_RB=y`、
  `CONFIG_PROVE_LOCKING=y`、`CONFIG_DEBUG_ATOMIC_SLEEP=y`、
  `CONFIG_DEBUG_LIST=y`、`CONFIG_DEBUG_FS=y`、`CONFIG_KASAN=y`，并且
  `CONFIG_ARM64_PTR_AUTH` 未设置。
- `/Users/yhome/workspace/source_code/linux-5.10.29/` 是只读参考源码树，用于
  核对声明、导出状态、调用上下文和结构体布局。
- 实际模块必须与用于启动 QEMU 的 5.10.29 内核保持版本、架构、配置和
  `vermagic` 一致。
- 禁止使用 `kallsyms_lookup_name()` 或 `kprobe_lookup_name()` 绕过导出边界。
- 禁止在 macOS 或 Lima guest 直接加载实验模块。
- 每个 runtime probe 使用全新的 QEMU guest；不同实验之间不共享内核状态。

## Chosen Approach

采用“清单驱动、单项编译、单项启动、单项归档”的方案。

每个 probe 只验证一个接口或一个紧密相关的接口组合。编译/链接失败直接归类
为 `NO-SYMBOL` 并保存 `modpost` 日志；构建成功的 probe 才进入 QEMU，运行时
再根据报告、对照数据、dmesg 和清理状态判定。

探针可以共享公共辅助代码，但每次只编译一个 `PROBE` 编号，并在构建前清理
上一个 probe 的生成物。这样可以避免多个未导出符号混在一次 `modpost` 失败中，
也避免上一项实验的 `.o` 或 `Module.symvers` 污染当前结果。

不采用把所有实验放在一个运行时开关模块中的方案，因为它会引入构建产物和
实验状态的隐性共享。不采用动态符号查找，因为它违反项目约束并把清晰的链接期
结论变成不可靠的运行期行为。

## Components

### Temporary spike module

实验模块放在 `spike/`，不复用现有的 `kernel_module/criu_probe.c`。模块提供：

```text
spike/criu_spike.ko
/sys/kernel/debug/criu_spike/status
/sys/kernel/debug/criu_spike/report
/sys/kernel/debug/criu_spike/vmas
```

模块参数包括：

```text
target_pid=<PID>
anon_addr=<address>
shared_addr=<address>
guard_addr=<address>
insert_addr=<address>
probe=<probe-id>
```

报告使用稳定的 `key=value` 文本格式，使 guest 脚本可以解析并将内容同时
保存在证据目录中。

### Known-layout target

`tests/progs/known-layout.c` 是一个静态链接 ARM64 目标程序，创建并输出：

- 私有匿名页，填充 `0xa5`；
- 共享匿名页，填充 `0x5a`；
- `PROT_NONE` guard page；
- 未 fault 的普通匿名插入候选页，用于 `vm_insert_page()`；
- PID 和各区域地址。

程序持续运行，直到 guest 实验脚本结束。额外的插入候选页只供 `4.8` 使用，
其他实验忽略它。

### Test orchestration

实验清单位于 `tests/s0/probes.tsv`，至少记录编号、类型、符号、预期状态、
关键门禁和地址需求。编排器包括：

```text
tests/s0/run-all.sh
tests/s0/run-one.sh
tests/s0/guest-run-one.sh
tests/s0/check.sh
tests/s0/summarize.sh
```

`run-all.sh` 检查环境、创建 run 目录、遍历清单并汇总结果。`run-one.sh` 对
compile probe 只运行构建，对 runtime probe 构建后调用现有
`scripts/run-qemu.sh --ci --script ...`。`guest-run-one.sh` 在 QEMU 内启动
目标程序、加载模块、读取 debugfs 和 `/proc` 对照、检查 dmesg、卸载模块，
最后输出 `S0_RESULT: <id>:<status>`。

现有 `scripts/run-qemu.sh` 保持通用，不加入 S0 专属分支。

## Probe Matrix

### Group 1: task lookup

| ID | Experiment | Method | Expected |
|---|---|---|---|
| 1.1 | `find_get_task_by_vpid()` | 独立编译/链接 | `NO-SYMBOL` |
| 1.2 | `pid_task(find_vpid(nr), PIDTYPE_PID)` | RCU 保护下运行并比对 PID、TGID、comm | `OK` |
| 1.3 | `get_pid_task()` / `put_task_struct()` | 获取引用、读取 task、释放引用、重复卸载 | `OK` |
| 1.4 | 缺少 `rcu_read_lock()` | 故意违反 RCU 契约并观察 lockdep/RCU | `UNSAFE` |

组 1 至少要有 `1.2` 或 `1.3` 成功，否则 A 轨不可行。

### Group 2: address space and VMA

| ID | Experiment | Method | Expected |
|---|---|---|---|
| 2.1 | `mmget_not_zero()` | 独立编译/链接 | `NO-SYMBOL` |
| 2.2 | `get_task_mm()` / `mmput()` | 获取 mm、验证 `mm_users`、释放引用 | `OK` |
| 2.3 | `mmap_read_lock(mm)` | 持读锁做最小只读访问并检查 lockdep | `OK` |
| 2.4 | `mm->mmap` / `vm_next` | 遍历 VMA，和 `/proc/PID/maps` 逐项比对 | `OK` |
| 2.5 | `vm_file` / `d_path()` | 输出文件映射路径并与 procfs 比对 | `OK` |

`2.4` 是核心门禁。VMA 数量、起止地址、权限和两个 magic byte 必须全部一致，
且不能有 debug kernel 告警。

### Group 3: user page access

| ID | Experiment | Method | Expected |
|---|---|---|---|
| 3.1 | `follow_page()` | 独立编译/链接 | `NO-SYMBOL` |
| 3.2 | `get_user_pages_remote()` | 持要求的 mmap lock，获取一页、读内容、`put_page()` | `OK` |
| 3.3 | `access_process_vm()` | 读取私有页和共享页并比对 magic byte | `OK` |
| 3.4 | 未映射地址 | 读取一个确认未映射地址 | 错误返回且无异常 |
| 3.5 | `PROT_NONE` guard page | 分别记录 GUP 和 access 行为 | 可预测且无异常 |

`3.2` 或 `3.3` 至少一个成功。`3.4` 和 `3.5` 即使返回拒绝，也不能出现
oops、BUG、KASAN 或 lockdep 告警。

### Group 4: restore boundary and side effects

| ID | Experiment | Method | Expected |
|---|---|---|---|
| 4.1 | `mm_alloc()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.2 | `vm_area_alloc()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.3 | `insert_vm_struct()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.4a | `do_mmap()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.4b | `vm_mmap()` | 链接并记录作用域 | 可链接但只操作 `current->mm` |
| 4.5a | `do_munmap()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.5b | `vm_munmap()` | 链接并记录作用域 | 可链接但只操作 `current->mm` |
| 4.6 | `alloc_pid()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.7 | `kernel_execve()` | 独立编译/链接 | `NO-SYMBOL` |
| 4.8 | `vm_insert_page()` | 插页前后比较 flags 和 smaps | `UNSAFE` |

`4.8` 必须在正确的 `mmap_write_lock(mm)` 上下文中调用。即使函数返回 0，
普通匿名 VMA 被设置 `VM_MIXEDMAP` 也必须记为 `UNSAFE`。

`vm_mmap()` 和 `vm_munmap()` 的“可链接”不能被解释为目标 task 地址空间操作
能力；本地 5.10.29 源码中的实现通过 `current->mm` 工作，这个边界必须进入
结论表。

## Result Semantics

内部结果状态：

```text
OK
UNSAFE
NO-SYMBOL
WRONG-VALUE
CRASH
CLEANUP-FAIL
ENV-MISMATCH
```

最终文档仍使用 S0 要求的 `OK`、`UNSAFE`、`NO-SYMBOL`。其中
`WRONG-VALUE`、`CRASH` 和 `CLEANUP-FAIL` 作为失败原因归入 `UNSAFE` 的说明。
`ENV-MISMATCH` 不计入技术结论，必须修复环境后重跑。

compile probe 只有在目标符号明确由编译器或 `modpost` 报告未解析时才能记为
`NO-SYMBOL`。声明错误、普通编译错误和测试框架错误必须单独修复。

对于 `1.4`，检测到预期的 RCU 警告属于该实验的证据；它记录为 `UNSAFE`，不能
被基础设施检查误报成测试框架故障。

## Evidence

每次完整运行创建：

```text
artifacts/s0/<run-id>/
├── summary.tsv
├── environment.txt
├── source-audit.txt
└── <probe-id>/
    ├── build.log
    ├── modpost.log
    ├── qemu.log
    ├── report
    ├── vmas
    ├── proc-maps
    ├── proc-smaps
    ├── dmesg-before
    ├── dmesg-after
    └── result.txt
```

compile probe 没有的文件可以省略。结果文件至少包含：

```text
probe=2.4
status=OK
kernel=5.10.29
arch=arm64
config_hash=<hash>
reason=<short explanation>
```

证据目录默认不提交 Git。最终只提交汇总结论、日期、内核版本、架构、关键
配置和必要的错误摘要。

## Safety And Cleanup

每个 guest 实验维护 dmesg 起始位置，只检查本次实验新增内容。以下关键字
导致 runtime probe 失败：

```text
BUG:
WARNING:
Oops
panic
KASAN
use-after-free
possible circular locking
sleeping function called
suspicious RCU usage
```

guest 使用 trap 尝试卸载模块、结束目标进程、保存报告并清理 debugfs。没有
`S0_RESULT` 时，宿主根据 QEMU 日志归类为 `CRASH`。实验失败不自动重试，避免
把非确定性错误隐藏起来。

完成 S0 后删除 spike 生成物和一次性代码，不把它演化为 A1。保留设计文档、
实施计划和 `docs/principles/08-kernel-module-limits.md` 中的最终实验结论。

## Acceptance Criteria

- 组 1-3 每项都有可追溯的 `OK`、`UNSAFE` 或 `NO-SYMBOL` 结论；
- 组 1 至少一条 task 获取路径通过；
- `2.4` VMA 对照通过；
- `3.2` 或 `3.3` 至少一条页读取路径通过；
- 组 4 每项都确认预期符号/边界，`4.8` 保存副作用证据；
- 所有关键 runtime probe 的 dmesg 检查和模块清理通过；
- 结论表回填到 `docs/principles/08-kernel-module-limits.md`；
- spike 代码删除或移入明确标注的一次性目录；
- 现有 build/lint/QEMU smoke test 通过，且没有引入禁止的符号查找或旧版
  `mmap_sem` 用法。
