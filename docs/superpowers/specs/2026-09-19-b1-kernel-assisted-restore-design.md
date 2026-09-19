# B1 内核辅助 restore 设计

**状态：** 设计已确认，实施计划已生成  
**日期：** 2026-09-19  
**适用环境：** Lima `criu-dev` 构建，嵌套 Linux 5.10.29/aarch64 QEMU guest 验证

## 1. 目标与架构决策

B1 实现“真 CRIU 镜像 → 本项目 restore”的单进程最小闭环。用户空间负责读取
CRIU protobuf、语义校验、打开文件、创建载体进程、建立 staging VMA、填充页面和
准备 `rt_sigframe`；Linux 5.10.29 内核辅助接口负责最终地址空间提交；最后仍由
用户空间 bootstrap 触发标准 aarch64 `rt_sigreturn`。

```text
CRIU protobuf
    │
    ▼
用户空间 parser / validator / orchestrator
    │
    ├─ 打开并校验 backing file
    ├─ clone3(set_tid) 创建 carrier
    ├─ staging mmap + 页面填充
    ├─ bootstrap 与 sigframe 准备
    │
    ▼
/dev/criu_restore：VALIDATE → COMMIT
    │
    ├─ 内核复制并保存完整 restore plan
    ├─ 内核提交最终 VMA、清理旧地址空间
    └─ 成功后返回到 bootstrap 继续执行
    │
    ▼
bootstrap（纯汇编）
    ├─ 设置 TPIDR_EL0
    ├─ 将 SP 指向最终 stack 上的 sigframe
    └─ svc __NR_rt_sigreturn
    │
    ▼
恢复后的 PC / SP / PSTATE / 通用寄存器 / FPSIMD / signal mask
```

这是对原 B1 “全部由用户态执行 mremap”方案的有意修正。地址空间最终提交仍由
即将成为目标进程的 carrier 自己触发，但具体的 VMA 事务由内核完成，以便使用
Linux 内部的 `mm_struct`/VMA 操作而不让外置模块调用未导出的内部符号。

内核不解析 protobuf，也不负责普通文件路径解析、页镜像解码或寄存器语义解析。

## 2. CRIU 参照实现与本项目对应关系

设计依据不是抽象地“模仿 CRIU”，而是逐项对应 CRIU 当前源码的实际阶段。

| CRIU 位置 | 已确认的语义 | B1 对应 |
|---|---|---|
| `criu/criu/cr-restore.c:restore_one_alive_task()` | 先准备 fd、文件锁、VMA、信号、定时器、rlimit、proc 状态和 seccomp，再进入地址空间恢复 | 用户空间在 `COMMIT` 前完成本范围内所有可失败的 B 类恢复 |
| `criu/criu/mem.c:prepare_mappings()` | 先保留 premap 区，再逐个建立 staging VMA，最后填充私有页 | 用户空间 staging 阶段 |
| `criu/criu/mem.c:premap_private_vma()` | 文件私有 VMA 先 reopen；为后续 mremap 临时增加可写权限；guard page 需额外保留 | parser/validator 的 VMA 规则和 staging 规则 |
| `criu/criu/mem.c:restore_priv_vma_content()` | 依据 pagemap/page image 填充页，不把文件映射内容错误地当作匿名页 | 用户空间页填充；脏的 file-private 页首个 gate 拒绝 |
| `criu/criu/pie/restorer.c:vma_remap()` | source/target 重叠时先放置 guard page，再移到临时不重叠区域，最后移动到目标 | 内核 `COMMIT` 的 VMA 移动算法 |
| `criu/criu/pie/restorer.c:unmap_old_vmas()` | 清除旧地址空间，但必须跳过 premap 和 bootstrap | 内核提交阶段保留 staging/bootstrap，等 VMA 搬迁完成 |
| `criu/criu/pie/restorer.c:prepare_restorer_blob()` | restorer 独立映射，不能把自身简单当作普通目标 VMA 搬走 | B1 bootstrap 独立保留，不参与普通 VMA 移动 |
| `criu/criu/pie/restorer.c:restore_thread_common()` | `set_tid_address`、robust futex、调度信息、非 sigframe 寄存器、TLS 等在 sigreturn 前准备 | B1 先做支持范围内的线程辅助状态；TLS 由 bootstrap 显式写入 |
| `criu/criu/sigframe.c` | 组合通用寄存器、signal mask、FPU 和 signal stack 状态 | 用户空间构造最终 stack 上的 sigframe |
| `criu/criu/arch/aarch64/crtools.c:restore_gpregs/restore_fpu()` | 31 个通用寄存器、SP、PC、PSTATE、32 个 SIMD 寄存器、FPSR/FPCR、FPSIMD magic/size | B1 aarch64 sigframe 填充 |
| `criu/criu/arch/aarch64/include/asm/restorer.h:restore_tls()` | `msr tpidr_el0, value`，TLS 不由 rt_sigreturn frame 自动恢复 | bootstrap 在 `svc` 前执行 `msr tpidr_el0` |
| `criu/compel/arch/aarch64/src/lib/include/uapi/asm/sigframe.h` | `rt_sigframe`、`cr_sigcontext`、aux context 和 `ARCH_RT_SIGRETURN` 的精确布局 | B1 只使用该布局，不自行猜测结构大小 |

Linux 5.10.29 中 `arch/arm64/kernel/signal.c` 是最终的
`rt_sigreturn` 内核路径；B1 不复制或替代这条标准路径。

## 3. B1 首个核心 gate

### 3.1 必须支持

- Linux 5.10.29、aarch64、同一 QEMU guest；
- 真 CRIU 生成的单进程镜像；
- 单线程、无子进程、无共享 `mm`；
- 静态极简测试程序；
- 私有匿名 VMA：heap、stack、普通匿名区；
- 干净的私有 file-backed VMA；
- fd 0/1/2 为普通文件；
- aarch64 通用寄存器、SP、PC、PSTATE、TLS、FPSIMD；
- `rt_sigreturn` 恢复 signal mask；
- `clone3(set_tid)` 精确恢复 guest 内目标 PID；
- 恢复后 PID 存活、tick 继续增长、heap/stack marker 完整；
- 恢复后 `/proc/$pid/maps` 与目标布局一致；
- guest `dmesg` 没有 Oops、BUG、WARNING、refcount splat 或 KASAN 报告。

测试使用固定地址策略或同一 guest 的等价地址布局。vDSO/vvar 不做跨地址重定位；
若目标 vDSO 地址与当前 guest 不一致，必须在不可逆提交前明确返回
`-EOPNOTSUPP`。

### 3.2 首个 gate 明确排除

- 多线程、进程树、session/pgid；
- pipe/socket、共享 fd table、共享 shmem；
- signal disposition、pending signal、timer；
- cgroup、PID/mount/network/user namespace；
- cwd/root 和挂载盘重建；
- file-backed `MAP_SHARED`、memfd/POSIX shm、SysV shm；
- vDSO 跨地址重定位；
- PAC/SVE/GCS 扩展；
- dirty file-backed private pages；
- credentials、seccomp、LSM 上下文。

排除项必须在 parser/validator 阶段或 `VALIDATE` 阶段得到可诊断的
`-EOPNOTSUPP`，不能生成一个看似成功但语义不完整的进程。

## 4. restore 状态机

### 4.1 用户空间阶段

1. **读取镜像**：读取 `inventory`、`core`、`mm`、`vma`、`pagemap`、`pages`、
   `ids`、`fs`、`creds` 等本范围需要的 CRIU image。
2. **完整验证**：检查 protobuf 字段、数量上限、页对齐、VMA 不重叠、地址范围、
   目标架构、PID、VMA kind、文件身份、vDSO 地址和不支持能力。
3. **准备文件**：在用户空间打开 file-backed VMA 对应的文件，并用 `fstat` 校验
   设备号、inode、文件大小及镜像记录的稳定身份。文件移动后若不能解析到同一稳定
   对象，提前失败；不在内核中按路径重新打开。
4. **创建 carrier**：父进程使用 `clone3(set_tid)` 创建目标 PID 的单进程 carrier。
   carrier 是之后所有地址空间操作的 `current`。
5. **恢复本范围 B 类状态**：恢复普通 fd、rlimit、支持的 signal mask 和
   `set_tid_address`/robust-list 等辅助状态。任何可能需要原始 restore 地址空间的
   操作都必须在 `COMMIT` 前完成。
6. **建立 staging**：在 carrier 当前地址空间中，把目标 VMA 映射到与目标区间不重叠
   的临时范围；file-private VMA 使用已验证的 fd，匿名 VMA 使用匿名映射；按
   pagemap/page image 填充页面。
7. **准备 bootstrap**：复制纯汇编 bootstrap 和参数到独立映射，准备最终 stack 上
   的 aarch64 `rt_sigframe`。之后由 bootstrap 代码发起 `COMMIT`，使系统调用返回地址
   本身位于 bootstrap，而不是旧 libc/旧栈。

### 4.2 `VALIDATE` 阶段

`VALIDATE` 是可失败、可诊断、不可改变目标地址空间的阶段。用户空间通过
`/dev/criu_restore` 提交一个版本化 `criu_restore_plan_v1`；内核执行：

- 检查 ABI version、结构大小、flags 和最大 VMA 数；
- `copy_from_user()` 一次性复制固定大小的 plan；
- 按 `vma_count` 分配内核内存并复制完整 VMA 数组；
- 检查每个 staging/target 区间的页对齐、长度溢出、地址上限、重复目标；
- 检查 staging 区间与 target 区间的重叠关系，并为需要的 guard/temporary move
  保留失败边界；
- 检查 bootstrap code/stack/sigframe 范围位于允许的保留区；
- 只接受 B1 支持的 VMA kind、权限和映射 flags；
- 检查 file-private VMA 已由用户空间建立 staging 映射，且没有 dirty-page 标记；
- 检查 vDSO/vvar 的“同地址保留”约束；
- 将完整 plan 固化到 transaction context。

`VALIDATE` 之后，内核不再保留或追踪用户空间数组指针。

### 4.3 `COMMIT` 阶段

成功进入 `COMMIT` 后不再允许回滚。内核只使用 `VALIDATE` 时保存的 plan，并在
发起 ioctl 的 carrier（即 `current`）上执行：

1. 冻结本 transaction，拒绝重复 `COMMIT`；
2. 保留 bootstrap 和 staging 区间；
3. 等价执行 CRIU `unmap_old_vmas()`，清理旧地址空间中不属于保留区的映射；
4. 按 CRIU 的低地址/高地址顺序移动目标 VMA；
5. 对 source/target 重叠的单个 VMA 使用 guard page + 中间临时区算法；
6. 让所有 VMA 移动成功后，恢复最终 VMA 权限，避免 staging 阶段的临时
   `PROT_WRITE` 泄漏到目标进程；
7. 保留 bootstrap 直到 `rt_sigreturn` 完成，不能提前 `munmap` 自身；
8. 成功从 ioctl 返回到 bootstrap 的下一条指令。

`COMMIT` 不再次读取用户指针，也不解析 protobuf。若提交已经开始后某一步失败，
内核返回错误没有意义，carrier 必须按失败路径退出，由父进程使用记录下来的 PID
逐个清理并 `waitpid()`；不能依赖进程组或假设 restore 会自动回滚。

## 5. 内核辅助 ABI

### 5.1 设备与事务

B1 新增独立 misc device：

```text
/dev/criu_restore
```

每次 `open()` 对应一个 restore transaction。父进程在 `clone3` 前打开设备并完成
`VALIDATE`；carrier 继承该文件描述符，在 bootstrap 中发起 `COMMIT`。事务状态为：

```text
OPEN → VALIDATED → COMMITTING → COMMITTED
                         └──────→ FAILED
```

`COMMITTED` 或 `FAILED` 事务都不能再次提交。父进程必须保持文件描述符有效到
carrier 报告成功或退出。

### 5.2 版本化结构

实现计划必须先锁定 ABI 头文件，建议路径为 `include/criu_restore_abi.h`。字段采用
固定宽度整数；数组和字符串不以内嵌用户指针形式存入内核 transaction。

```c
#define CRIU_RESTORE_ABI_VERSION 1U
#define CRIU_RESTORE_MAX_VMAS    4096U

enum criu_restore_vma_kind {
	CRIU_RESTORE_VMA_ANON_PRIVATE = 1,
	CRIU_RESTORE_VMA_FILE_PRIVATE = 2,
	CRIU_RESTORE_VMA_STACK        = 3,
	CRIU_RESTORE_VMA_VDSO         = 4,
};

struct criu_restore_vma_v1 {
	__u64 staging_start;
	__u64 target_start;
	__u64 length;
	__u32 prot;
	__u32 map_flags;
	__u32 kind;
	__u32 flags;
};

struct criu_restore_plan_v1 {
	__u32 version;
	__u32 size;
	__u32 vma_count;
	__u32 flags;
	__u32 target_pid;          /* carrier PID required at COMMIT */
	__u32 reserved0;
	__u64 vmas_user_ptr;       /* VALIDATE only; never used by COMMIT */
	__u64 bootstrap_code_start;
	__u64 bootstrap_code_end;
	__u64 bootstrap_pc;
	__u64 bootstrap_stack_start;
	__u64 bootstrap_stack_end;
	__u64 bootstrap_sp;
	__u64 sigframe_staging_sp;
	__u64 sigframe_final_sp;
	__u64 tls;
};
```

真实实现可以因 Linux 5.10.29 的 UAPI 约束调整字段，但必须保持以下不变量：

- `VALIDATE` 复制完整 VMA 数组；
- `COMMIT` 只读内核 transaction；
- `COMMIT` 只允许由 `target_pid` 对应的 carrier 调用；
- 不把路径、protobuf 指针或未验证的用户地址作为 COMMIT 输入；
- 所有地址和长度使用 `__u64`，内核内部再做架构相关范围检查；
- ABI version/size/最大数量检查先于任何地址空间操作。

`include/criu_restore_abi.h` 是此事务的固定 ABI：`VALIDATE` 使用
`struct criu_restore_plan_v1`，其中 `vmas_user_ptr` 只在 ioctl 执行期间有效；内核
必须复制 plan 和其固定宽度 VMA 数组到 per-open transaction。`COMMIT` 使用单独的
`struct criu_restore_commit_v1`，它只含 version、size、flags 和 reserved 字段，不能
携带 VMA、地址、路径或任何用户指针。COMMIT 只能读取 transaction 已拥有的副本。

未知 ABI version、过小结构、零 `target_pid`、未对齐或溢出的 VMA 区间、重复 target
和超过 `CRIU_RESTORE_MAX_VMAS` 的输入必须在 `VALIDATE` 返回 `-EINVAL`；已知但不在
B1 gate 内的 VMA kind、flags 或语义必须返回 `-EOPNOTSUPP`。这一边界不允许内核解析
protobuf，也不允许将不支持的恢复伪装为成功。

### 5.3 权限与能力

设备节点只允许受控 restore 用户打开；实现至少要求 `CAP_SYS_ADMIN`，并在
`VALIDATE`/`COMMIT` 阶段拒绝非目标进程或重复提交。B1 不通过现有 debugfs
控制文件承载事务，也不从外置模块直接调用 `mm_alloc()`、`do_mmap()`、
`mremap_to()` 等未导出的内部函数。需要的内核能力通过 Linux 5.10.29
`patches/linux-5.10.29/` 中独立的内核补丁提供，并由 guest 内核构建验证。

## 6. VMA 与页面语义

### 6.1 staging 与最终 VMA

用户空间按 CRIU `prepare_mappings()` 的语义建立 staging：

- staging 起始地址由用户空间选择并显式检查，不能与任何最终 target 重叠；
- 匿名私有页使用 `MAP_PRIVATE|MAP_ANONYMOUS`；
- 干净 file-private 页使用已验证的 backing fd；
- 为后续 mremap 暂时增加写权限，但 COMMIT 成功前后都要恢复真实权限；
- grow-down stack 的 guard page 必须作为独立保留范围处理；
- 额外的页镜像映射使用 `pread` 读入已存在的 staging 页，不能用额外
  `mmap` 把 image file 留在待清理地址空间中。

### 6.2 source/target 重叠

不能假设所有 VMA 的 staging source 到 target 都天然不重叠。内核 COMMIT 必须：

- 先按地址方向分组，保持与 CRIU `vma_remap()` 一致的搬迁顺序；
- 发现单个 source/target 区间重叠时，先在目标边界放置 guard page；
- 将 source 搬到临时不重叠地址；
- 再执行最终 `MREMAP_FIXED|MREMAP_MAYMOVE`；
- 任一步失败都停止后续动作并让 carrier 退出，不伪造“已完成”。

### 6.3 file-backed private VMA

B1 只支持干净的 file-backed private VMA。用户空间必须在 `VALIDATE` 前：

- 根据 CRIU reg-file 信息解析并打开 backing file；
- 校验 device/inode、大小和所需 `pgoff`；
- 在 staging 中使用该 fd 建立映射；
- 对镜像中声明为 dirty/COW 的 file-private 页直接返回 `-EOPNOTSUPP`。

因此“dump 后文件被移动”不是内核 COMMIT 的隐式行为：如果用户空间不能解析到
同一个稳定文件对象，restore 在 `VALIDATE` 前失败；不能仅按新路径名恢复并宣称
语义等价。

## 7. aarch64 最后一跳

### 7.1 sigframe

用户空间根据 `criu/compel/arch/aarch64/.../sigframe.h` 构造最终 stack 上的
`struct rt_sigframe`：

- `uc.uc_mcontext.regs[31]`、`sp`、`pc`、`pstate` 来自 `core`;
- `uc.uc_sigmask` 来自支持范围内的 signal mask；
- `__reserved` 中写入 `fpsimd_context`，包含 32 个 `vregs`、`fpsr`、`fpcr`、
  `FPSIMD_MAGIC` 和正确的 `size`；
- 所有地址按 AArch64 要求 16-byte 对齐；
- sigframe 的最终地址必须属于被 kernel COMMIT 搬到目标地址的 stack VMA。

B1 不自行定义另一份“近似 sigframe”；结构布局、magic 和长度必须与 Linux 5.10.29
的 `arch/arm64/include/uapi/asm/sigcontext.h` 及 CRIU compel 定义一致。

### 7.2 bootstrap

bootstrap 是独立映射中的短汇编，不调用 libc，不使用旧栈，不访问已被 COMMIT
清理的 image 映射。成功 COMMIT 返回后，它按以下顺序执行：

```asm
mov sp, <sigframe_final_sp>
msr tpidr_el0, <tls>
mov x8, #__NR_rt_sigreturn
svc #0
```

`rt_sigreturn` 不返回。它负责原子恢复通用寄存器、SP、PC、PSTATE、signal mask
和 FPSIMD；TLS 不在 sigframe 中自动恢复，所以 `msr tpidr_el0` 是必需步骤。

## 8. 失败边界与清理

### 8.1 COMMIT 前

所有 parser、文件打开、VMA staging、页填充、bootstrap 拷贝、sigframe 校验和
`VALIDATE` 失败都必须返回可诊断错误；父进程负责关闭 fd、撤销 staging、删除
临时镜像并终止尚未进入不可逆阶段的 carrier。

### 8.2 COMMIT 开始后

地址空间可能已经部分搬迁，不能尝试回滚。内核只报告最早可报告的错误；用户空间
必须把 carrier PID 记录在清理表中，逐 PID `kill()` 并 `waitpid()`，同时检查
guest dmesg。不能使用 `kill(-pgid)` 作为唯一清理手段，因为 carrier 可能已经
改变 session/pgid 或阻塞在不可达同步点。

### 8.3 成功判定

以下条件必须全部满足才输出 B1 gate PASS：

1. restore 命令或 orchestrator 没有未处理错误；
2. 目标 PID 与镜像 PID 相同且 `kill -0` 成功；
3. 目标程序的 tick/heartbeat 继续增长；
4. heap/stack marker 与 dump 时一致；
5. `/proc/$pid/maps` 与预期 VMA 语义和地址一致；
6. TLS marker、寄存器 marker 或程序行为验证通过；
7. guest dmesg 无 Oops/BUG/WARNING/refcount/KASAN；
8. transaction fd 关闭后无残留 carrier、临时映射或半成品进程。

不能因为 `criu dump` 或 restore 进程返回 0 就宣称成功；必须检查恢复后的 PID
仍然存活并持续执行。

## 9. 测试与验收矩阵

### 9.1 用户空间合同测试

- protobuf 缺字段、错误架构、超限 VMA、整数溢出；
- VMA 未对齐、重叠、target 与 staging 冲突；
- unsupported VMA kind、dirty file-private、vDSO 地址变化；
- 缺失 backing file、device/inode 不匹配；
- sigframe magic/size/alignment 错误；
- `VALIDATE` 后修改用户数组不会影响 `COMMIT`（TOCTOU 合同）；
- 事务重复 `VALIDATE`/重复 `COMMIT` 被拒绝。

### 9.2 Linux 5.10.29 guest 合同测试

- misc device 可打开且只接受正确 ABI version；
- `VALIDATE` 失败时目标 carrier 地址空间未改变；
- COMMIT 成功后 bootstrap 仍可执行；
- source/target 重叠路径覆盖 guard page 和临时搬迁；
- COMMIT 中途失败时 carrier 可被父进程逐 PID 清理；
- rmmod、再次加载和 dmesg 检查均干净。

### 9.3 端到端 gate

测试入口必须使用 guest-local `/tmp`，不能把 Lima 9p 工程目录作为 restore 工作目录：

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/b1-kernel-assisted-restore.sh'
```

预期标志为：

```text
B1_KERNEL_ASSISTED_RESTORE: PASS
```

## 10. 与既有经验的强制一致性

实施时必须遵守 [A3-问题与解决方法复盘](../../A3-问题与解决方法复盘.md)：

- 仍然使用 CRIU protobuf；内核只输出/消费结构化 ABI，不在内核中生产 protobuf；
- 不把 `criu restore` 的退出码当作进程恢复成功；
- 严格区分 macOS、Lima 构建环境和 Linux 5.10.29 QEMU guest；
- 不把 Lima 9p 路径用作 guest restore 工作目录；
- 所有 unsupported 场景显式拒绝，不用 allowlist 或伪造字段掩盖；
- 每个 gate 后检查恢复 PID 的存活、行为和 guest dmesg；
- 不直接使用外置模块无法链接的 `mm_alloc()`、`do_mmap()`、`mremap_to()` 等内部符号；
- 保留失败证据、清理证据和复现命令，避免只记录“命令返回 0”。

## 11. 后续实施计划的边界

本文是 B1 的设计合同，不是代码实施计划。正式实施计划必须在本设计确认后另行
生成，并按以下顺序拆分：

1. 锁定 UAPI/transaction ABI 与 parser contract；
2. 实现 Linux 5.10.29 内核 patch 和 misc device；
3. 实现用户空间 image parser、staging 和 file validation；
4. 实现 bootstrap/sigframe/TLS；
5. 实现失败清理与负向合同测试；
6. 在同一 aarch64 guest 完成端到端 gate；
7. 运行 verification-before-completion 后再决定是否合入 main。

在实施计划生成并获用户批准前，不开始写 B1 代码。
