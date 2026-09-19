# A6 验证记录

日期：2026-09-19

环境：macOS 宿主机，Lima `criu-dev` 构建 ARM64 模块和静态 fixture，嵌套
Linux 5.10.29/aarch64 QEMU guest 执行 `insmod`、dump、converter 和真实 CRIU
restore。CRIU 验证器来自 `/Users/yhome/workspace/source_code/criu_module/criu`。

通过的 host-side 检查（均在 Lima `criu-dev` 中执行）：

- `make -C kernel_module KDIR=/home/yhome.guest/kernels/linux-5.10.29`
- `make -C userspace/criu-module-convert clean all LDFLAGS=`
- `make -C tests/progs clean all`
- `tests/a6-abi-contract.sh`
- `tests/a6-converter-images.sh`
- `tests/a6-unsupported.sh`
- `tests/snapshot-format.sh`
- `tests/converter-format.sh`
- `tests/a5-converter-fds.sh`
- `tests/dump-task-contract.sh`

`dump-task-contract.sh` 使用系统自带的 `grep -Eq`，不再依赖 Lima
环境中未预装的 `rg`。

真实 guest 门禁使用：

```bash
CRIU_SOURCE=/Users/yhome/workspace/source_code/criu_module/criu \
  ./scripts/run-qemu.sh --ci --script tests/a6-cross-restore.sh
```

`run-qemu.sh` 会把 ARM64 CRIU 二进制和 `crit`/`lib` staging 到 nested
guest；如果没有提供 `CRIU_SOURCE` 且 worktree 不含 CRIU checkout，门禁会
按设计返回 `SKIP: CRIU unavailable`，不能据此声称 cross-restore 已通过。

最终通过的 guest 门禁：

最后一项输出：

```text
A6_HANDLERS: PASS (restore, behavior, liveness)
A6_PENDING: PASS (restore, behavior, liveness)
A6_TIMERS: PASS (restore, behavior, liveness)
A6_CROSS_RESTORE: PASS
```

期间修正了两个真实 ABI/语义问题：ITIMER 序列化顺序必须是 REAL、VIRTUAL、PROF；
converter 将内核纳秒剩余时间截断到 CRIU 所需的微秒精度。pending 和 timer fixtures
也安装了相应 handler，避免默认信号动作或恢复期间输出改变已被 CRIU 打开的文件。

未纳入本记录的项目：完整 18 项扩展用例、全量 ZDTM，以及 A3/A4/A5 的全量回归。
