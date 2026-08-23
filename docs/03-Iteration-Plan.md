# CRIU 内核模块 —— 迭代计划(大纲)

> **执行者须知:** 本文件只是大纲。每个步骤的完整设计、CRIU 参照实现、测试方法在
> `docs/steps/` 下的独立文件里。技术原理(学习资料)在 `docs/principles/` 下。
> 开发环境与 CI 在 `docs/04-Dev-Environment.md`。

**目标:** 用内核模块实现 CRIU 的 **dump 侧**,并用一个用户空间 mini-restore 学习
restore 侧;每一步都能独立验证,不依赖后续步骤存在。

**架构:** 双轨并行。A 轨 = 内核模块 dump,验证器是**真 criu restore**。
B 轨 = 用户空间 mini-restore,验证器是**真 criu dump 出来的镜像**。两轨接口是
磁盘上的镜像文件,因此互不阻塞,任一轨单独完成都是完整成果。

**技术栈:** C(Linux Kernel Coding Style)、Linux 5.10.29 内核模块、protobuf-c、
QEMU/virtme-ng、GitHub Actions、CRIU 的 `crit` 与 ZDTM 测试套件。

**依据文档:** `docs/01-CRIU-Architecture-Analysis.md`(CRIU 现状分析)、
`docs/02-Kernel-Module-Design-Outline.md`(原始设计大纲,本计划替换其第六部分)。

---

## Global Constraints(全局约束,每个步骤都隐含包含)

| 约束 | 精确值 |
|---|---|
| 目标内核 | **Linux 5.10.29**。6.1+ 用 maple tree 取代了 VMA 链表,`vma->vm_next` 不存在 |
| 架构 | **x86_64 only**。`rt_sigframe` 与寄存器布局是 arch-specific |
| 代码风格 | Linux Kernel Coding Style:tab = 8 字符,行宽首选 80、上限 120 |
| 大括号 | 函数的 `{` 独占一行;`if`/`for`/`while`/`switch` 的 `{` 跟在行尾 |
| 注释 | 只用 `/* ... */`,多行注释每行以 ` * ` 开头 |
| 镜像格式 | 与 CRIU **二进制兼容**。以 `criu_module/criu/images/*.proto` 为唯一规范 |
| 模块许可 | `MODULE_LICENSE("GPL")`。非 GPL 拿不到大量 `EXPORT_SYMBOL_GPL` 符号 |
| 权限 | 所有入口检查 `capable(CAP_SYS_ADMIN)`,失败返回 `-EPERM` |
| 禁止 | 不使用 `kallsyms_lookup_name()` 绕过未导出符号。做不到就记录为约束 |
| 提交信息 | 按 `criu/CLAUDE.md`:正文后加 `Assisted-by: AGENT_NAME:MODEL_VERSION`,**不代签 `Signed-off-by`** |

---

## 为什么不按原大纲的 Phase 1-4 切

原大纲按**功能复杂度**切(单线程 → 多线程 → 进程树 → namespace)。每一片都是一个
完整垂直切片,片内九个环节(框架、控制接口、冻结、内存 dump、序列化、进程创建、
内存 restore、寄存器 restore、最后一跳)全部串联。后果:

1. **前三周没有任何一件事能单独验证。**
2. Phase 1 依赖的「内核里创建用户进程 + 安装地址空间」在 out-of-tree 模块里
   **做不到**(已核实,见 `docs/principles/08-kernel-module-limits.md`),会在第
   三周才撞上地基不成立。

本计划改按**可验证性**切,并把「做不到」的探测提前到第 1 周。

---

## 总体步骤

### 前置

| 步骤 | 名称 | 工期 | 产出 | 文件 |
|---|---|---|---|---|
| **S0** | 可行性打靶(代码可丢弃) | 1 周 | 「什么能用/不能用」结论表 | [S0](steps/S0-feasibility-spike.md) |

### A 轨 —— 内核模块 dump(验证器:真 criu restore)

| 步骤 | 名称 | 工期 | 产出 | 文件 |
|---|---|---|---|---|
| **A1** | 只读探针 + 对照 diff | 1-2 周 | 能读出 VMA 列表,与 `/proc` 逐字段一致 | [A1](steps/A1-readonly-probe.md) |
| **A2** | 冻结 / 解冻 | 1 周 | 可靠冻结多线程进程,无信号丢失 | [A2](steps/A2-freeze.md) |
| **A3** | **极简进程完整 dump(里程碑)** | 2-3 周 | `criu restore` 能恢复本模块产出的镜像 | [A3](steps/A3-minimal-dump.md) |
| **A4** | 多线程 | 1-2 周 | 每线程 `core-$tid.img` | [A4](steps/A4-threads.md) |
| **A5** | 文件描述符(pipe → 常规文件 → unix socket) | 2-3 周 | `files.img` / `fdinfo-*.img` | [A5](steps/A5-fds.md) |
| **A6** | 信号与定时器 | 1-2 周 | `sigacts-*.img` / `timer*.img` | [A6](steps/A6-signals-timers.md) |
| **A7** | 进程树 + session/pgid | 1-2 周 | 多进程 `pstree.img` | [A7](steps/A7-pstree.md) |
| **A8** | 共享资源去重 | 1-2 周 | 跨任务共享的 pipe / SHM 只存一份 | [A8](steps/A8-shared-resources.md) |

A 轨合计 **10-17 周**。A3 是**门禁**:A3 不通过,A4-A8 全部无意义。

### B 轨 —— 用户空间 mini-restore(验证器:真 criu dump 的镜像)

| 步骤 | 名称 | 工期 | 产出 | 文件 |
|---|---|---|---|---|
| **B1** | 单进程 restore(镜像读取 + 地址空间偷换 + `rt_sigreturn`) | 2-3 周 | 能恢复极简进程 | [B1](steps/B1-mini-restore.md) |
| **B2** | 进程树 restore(两趟 fork + session/pgid) | 2-3 周 | 能恢复多进程树 | [B2](steps/B2-pstree-restore.md) |

B 轨可与 A 轨**任意并行**,只依赖 S0。

### X 轨 —— 只有内核能做的事

| 步骤 | 名称 | 工期 | 产出 | 文件 |
|---|---|---|---|---|
| **X1** | 恢复 `task->start_time` 的内核接口 | 1 周 | 补上 CRIU 已知空洞,可上游 | [X1](steps/X1-start-time.md) |

---

## 依赖图

```
S0 ──┬──► A1 ──► A2 ──► A3(门禁)──┬──► A4 ──┐
     │                              ├──► A5 ──┼──► A8
     │                              ├──► A6 ──┤
     │                              └──► A7 ──┘
     │
     ├──► B1 ──► B2
     │
     └──► X1(任何时候都能做,不依赖 A/B)
```

A4/A5/A6/A7 之间**互不依赖**,可任意顺序、可并行。
建议排序依据:**哪一类能解锁最多 ZDTM 测试就先做**,让「下一步做什么」变成
可查的问题而不是要猜的问题(方法见 A3 的「排序仪表盘」一节)。

---

## 测试策略总览

三种手段,强度递增。每个步骤文件的「如何测试」一节会说明它用哪几种。

| 手段 | 做法 | 强度 | 最早可用 |
|---|---|---|---|
| **对照 diff** | 本模块 dump 进程 P → 真 criu 也 dump P → `crit decode` 后逐字段比 | 弱,但极早 | A1 |
| **交叉恢复** | 本模块 dump → **真 criu restore**。restore 成功即 dump 正确 | 强,端到端 | A3 |
| **ZDTM** | 用 shim 冒充 criu 二进制,跑官方 489 个静态测试 | 最强,回归防护 | A3 之后 |

三个已核实的现成资产:

1. `criu/images/` 下 **76 个 `.proto`** —— 完整镜像格式规范,不用自己发明
2. `crit decode`(`criu/crit/crit/__main__.py:1608`)—— 二进制镜像转 JSON,现成校验器
3. `criu/test/zdtm.py` 接受 `--criu-bin`(`criu/test/zdtm.py:3083`),
   `criu/test/zdtm/static/` 下有 **489** 个 `.c` 测试程序

---

## 技术原理(学习资料)

`docs/principles/` 下 9 篇。它们按**机制**组织,不按步骤组织 —— 因为同一个机制
往往被多个步骤用到,而按步骤组织会导致重复。

| # | 文件 | 讲什么 | 被哪些步骤引用 |
|---|---|---|---|
| 01 | [进程解剖](principles/01-process-anatomy.md) | **A/B 二分**、「载体全新内容全等」、`task_struct` 地图 | S0、A1 |
| 02 | [怎么让进程停下来](principles/02-freezing.md) | `SIGSTOP` 的三个致命问题、`PTRACE_SEIZE`、cgroup freezer | A2 |
| 03 | [地址空间与 VMA](principles/03-memory-and-vma.md) | 两个正交轴 → 四种 VMA、哪些页要存、premap→mremap | A1、A3、A8 |
| 04 | [镜像格式](principles/04-image-format.md) | 为什么照抄 CRIU 格式、protobuf 的两个陷阱、内核里做编码 | A3、B1 |
| 05 | [寄存器与 sigframe](principles/05-registers-and-sigframe.md) | `task_pt_regs`、`orig_ax`、**`rt_sigreturn` 那三条指令** | A3、A4、A6、B1 |
| 06 | [pid、会话、进程组](principles/06-pid-and-session.md) | `clone3(set_tid)`、**两遍 fork 的必然性**、结构性 vs 同步性 | A4、A7、B2 |
| 07 | [fd 与共享对象](principles/07-fd-and-shared-objects.md) | 三层间接、`kcmp` vs 比较指针、pipe 两端配对 | A5、A8 |
| 08 | [内核模块的边界](principles/08-kernel-module-limits.md) | 四个真实优势、三个 restore 障碍、「不要用」清单 | S0、X1 |
| 09 | [restore 的顺序](principles/09-restore-ordering.md) | 三种顺序类型、CRIU 八个阶段、失败清理 | A7、B1、B2 |

读法建议:

- **只想理解 C/R 是怎么回事** → 01 → 03 → 05 → 09。这四篇是主线
- **要动手写 dump** → 01 → 02 → 03 → 04
- **要动手写 restore** → 09 → 06 → 05
- **想知道这个项目为什么长这样** → 08,它解释了内核/用户态的分界

**08 的「实验复核」一节是空表,由 S0 填写。** 在那之前,所有「未导出」的结论都
只有 grep 依据。

---

## 与原大纲的差异说明

| 原大纲 | 本计划 | 原因 |
|---|---|---|
| Phase 1 含 restore | restore 移出内核(B 轨,用户空间) | 4 个 restore 必需原语内核模块没有 |
| 自定义二进制格式(7.3 节) | 复用 CRIU protobuf 格式 | 复用格式才能拿到 `criu restore` 和 ZDTM 当验证器 |
| `kthread_create()` 造进程(5.2 节) | 不做 | 造出的 task 父亲是 kthreadd、不在任何 session,无法事后纠正 |
| 镜像里存 `struct page *` / `pfns`(3.2 节) | 存页内容字节 | 指针和 PFN 跨重启无意义 |
| 「零拷贝」性能目标(9.1 节) | 目标改为「消除 parasite 与 /proc 文本解析」 | CRIU 已有 `vmsplice(SPLICE_F_GIFT)` 近零拷贝 |
| 「内存开销 < 10%」 | 删除 | 与「pin 住页面」直接矛盾,最坏 100% |
| Session/进程组放 Phase 3 | 提到 A7,且其约束在 A3 就必须体现 | 它决定 A3 的架构对不对,不是后加功能 |
