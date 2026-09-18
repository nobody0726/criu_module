# A6 验证记录

日期：2026-09-18

环境：macOS 宿主机，Lima `criu-dev` 构建 ARM64 模块和静态 fixture，嵌套
Linux 5.10.29/aarch64 QEMU guest 执行 `insmod`、dump、converter 和真实 CRIU
restore。CRIU 验证器来自 `/Users/yhome/workspace/source_code/criu_module/criu`。

通过的定向检查：

- `make -C kernel_module KDIR=/home/yhome.guest/kernels/linux-5.10.29`
- `tests/a6-abi-contract.sh`
- `tests/a6-unsupported.sh`
- `tests/snapshot-format.sh`
- `tests/converter-format.sh`
- `tests/a5-converter-fds.sh`
- `scripts/run-qemu.sh --ci --script tests/a6-cross-restore.sh`

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
