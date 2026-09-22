# B1 kernel-assisted restore verification

Date: 2026-09-22

## Verdict

B1 的首个核心 gate 已完成并通过。真实 CRIU 单进程镜像在 Linux 5.10.29/aarch64
QEMU guest 中完成用户空间解析、staging、内核 `VALIDATE -> COMMIT`、bootstrap
和 `rt_sigreturn`，恢复后的目标 PID 保持存活。

权威命令（必须通过 Lima 进入 guest，再由 guest 启动 QEMU）：

```sh
limactl shell criu-dev bash -lc \
  'set -o pipefail; \
   export CRIU_SOURCE=/home/yhome.guest/kernels/verify-a3-task5/criu; \
   cd /Users/yhome/.codex/worktrees/b1-kernel-assisted-restore/criu_module; \
   ./scripts/run-qemu.sh --ci --script tests/b1-kernel-assisted-restore.sh'
```

关键结果：

```text
B1_KERNEL_ASSISTED_RESTORE: PASS pid=127 maps_bytes=637
```

这次 gate 的返回码为 0，并验证了精确 PID、恢复后 `/proc/$pid/stat`、非空 maps
以及目标映射存在。guest 运行期间没有出现会使 gate 失败的 Oops、BUG、WARNING、
KASAN、refcount 或 use-after-free 日志。

## Root cause and fix

第一次真实 restore 到达 `COMMIT` 后仍立即退出。取证补丁记录到：

```text
B1_FORCE_SIGSEGV pid=127 sig=0 pc=7000000054 sp=7000002ff0
```

这不是 PID 创建失败，也不是页偏移、VMA `VM_EXEC`、I-cache 或 `rt_sigreturn`
寄存器恢复错误。mini-restore 自身的 glibc 在线程 TLS 中注册了 rseq；carrier 使用
`clone3(CLONE_SETTLS)` 切换到目标 TLS 时，内核仍保留从父线程继承的旧 rseq 指针。
`COMMIT` ioctl 返回后，内核的 `rseq_handle_notify_resume()` 访问该无效指针并调用
`force_sigsegv(0)`，所以 carrier 在 bootstrap 之前退出。

修复位于 `userspace/mini-restore/carrier.c`：clone 前记录父线程的 rseq 地址和大小，
child 在执行 bootstrap/restore entry 前用 `SYS_rseq` 和 `RSEQ_FLAG_UNREGISTER`
注销继承的旧 rseq 注册；同时 `CLONE_SETTLS` 明确设置目标 TLS。新增的
`tests/b1-carrier-contract.sh` 断言覆盖了这条契约，guest gate 验证了运行时效果。

取证阶段使用的 0009--0014 临时内核诊断补丁已删除；正式实现只保留运行时需要的
0007 bootstrap sigframe 和 0008 I-cache flush 补丁。

## Passing local contracts

以下契约在最终代码上通过：

- `git diff --check`
- `make -C userspace/mini-restore clean all`
- `sh tests/b1-restore-abi-contract.sh`
- `KDIR=/tmp/criu-module-b1-no-kernel sh tests/b1-kernel-patch-contract.sh`
- `sh tests/b1-validate-contract.sh`
- `sh tests/b1-vma-commit-contract.sh`
- `sh tests/b1-image-reader-contract.sh`
- `sh tests/b1-carrier-contract.sh`
- `sh tests/b1-staging-contract.sh`
- `sh tests/b1-sigframe-contract.sh`
- `sh tests/b1-cleanup-contract.sh`
- `sh tests/b1-negative.sh`
- `KDIR=/tmp/criu-module-b1-no-kernel sh tests/b1-patch-build.sh`
- `sh tests/b1-qemu-staging-contract.sh`

## Supported scope and explicit follow-up

本次 PASS 的范围仍是 B1 设计中定义的单进程、单线程、支持 VMA/page subset 的真实
CRIU 镜像。以下能力没有被本 gate 悄悄宣称支持，保留为后续任务：

- dirty file-private/COW、共享映射、复杂 file identity/reopen；
- 多线程、子进程树、namespace/cgroup/fs 关联资源；
- vDSO relocation、socket、定时器及其他 B2/B3 资源；
- 需要 live kernel 的 target-PID occupied 和 duplicate-COMMIT 负向场景；
- 扩展 ZDTM restore 矩阵和完整 maps/marker 对照覆盖。

这些限制仍通过 userspace validator 返回明确的 format/unsupported/io 诊断，不能被
零退出码或 parser dry-run 代替。
