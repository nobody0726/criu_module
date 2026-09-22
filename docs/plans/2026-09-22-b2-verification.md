# B2 process-tree restore verification

Date: 2026-09-22

## Scope

B2 核心只覆盖：

- 单线程 task；
- 每个 task 独立 `mm_struct`；
- `pstree.img` 中的 `pid/ppid/pgid/sid`；
- 两趟递归创建、session inheritance、pgid barrier；
- ready barrier 后统一进入 B1 `VALIDATE -> COMMIT`；
- 失败时 abort/wake 和逐 PID cleanup。

树协调 carrier 在执行递归 fork/session/pgid 逻辑期间保留父 TLS；只有进入
B1 bootstrap 后才安装目标 TLS。这一点延续了 B1 rseq/TLS 事故复盘，避免 glibc
协调代码在目标 TLS 尚未可用时运行。

多线程、共享 fd/socket/shmem、namespace、cgroup、mount/fs context、helper session
leader 和扩展 ZDTM 矩阵仍是 B2 backlog，不由本验证记录宣称完成。

## Local verification

已通过：

```text
make -C userspace/mini-restore clean all
sh tests/b1-negative.sh
sh tests/b1-cleanup-contract.sh
sh tests/b1-task-restore-contract.sh
sh tests/b2-pstree-contract.sh
sh tests/b2-shared-contract.sh
sh tests/b2-construction-contract.sh
sh tests/b2-pgid-contract.sh
sh tests/b2-negative.sh
git diff --check
```

这些测试覆盖 parser framing、重复 PID/缺失 leader、共享 scratch、超时、
跨分支 pgid leader、ready barrier、abort 唤醒和逐 PID 清理，但不等价于真实
kernel `COMMIT`。

## Authoritative guest gate

预期命令（必须从 Lima `criu-dev` 进入，再由 guest-local `/tmp` staging 的
`run-qemu.sh` 启动 Linux 5.10.29/aarch64）：

```sh
limactl shell criu-dev bash -lc \
  'set -o pipefail; \
   export CRIU_SOURCE=/home/yhome.guest/kernels/verify-a3-task5/criu; \
   cd /Users/yhome/.codex/worktrees/b2-process-tree-restore/criu_module; \
   ./scripts/run-qemu.sh --ci --script tests/b2-restore.sh'
```

实际执行结果（2026-09-23）：

```text
B2_PROCESS_TREE_RESTORE: PASS root=126 child=130
```

同时满足：

```text
B2_PROCESS_TREE_RESTORE: PASS
```

以及 root/child 的 `/proc/$pid/stat` 中 PPID、PGID、SID 与 dump 前一致，两个
个 PID 保持存活，且 gate 起始后的 dmesg 没有 Oops、BUG、WARNING、KASAN、
refcount、use-after-free 或 `criu_restore` 错误。B2 gate 不再使用 signal
handler marker，因为 signal disposition 尚未属于 B2 核心范围。

本次失败及修复记录：

1. 首次真实 dump 失败：CRIU 报告 `has rseq but kernel lacks
   get_rseq_conf feature`。fixture 改为使用
   `GLIBC_TUNABLES=glibc.pthread.rseq=0`，与 B1 guest gate 保持一致。
2. 每个 task 的 CRIU 镜像都被错误地拿 `pstree.img` 的首条 PID 校验。
   `b1_read_criu_images_for_pid()` 现在以所选 `core-/mm-/pagemap-` 文件的
   requested PID 为 task identity。
3. `--restore-sibling` 使用 `CLONE_PARENT` 时，Linux `clone3()` 要求
   `exit_signal=0`；carrier wrapper 已按该规则处理。
4. tree fixture 恢复点必须满足 B1 的栈 marker；同时 B1 bootstrap 的
   `target_stop` 兼容支持 `x2==0` 和旧的非零 flag 地址形式。

因此 B2 核心 guest gate 已通过；signal handler、fd/socket、共享内存、
namespace、cgroup、mount/fs context 和扩展 ZDTM 矩阵仍是后续任务。
