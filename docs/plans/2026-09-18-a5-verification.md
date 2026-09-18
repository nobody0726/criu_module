# A5 验证与问题修复记录

实现提交：`2717e08`（`codex/a5-fds`）。

## 环境与判据

2026-09-18 在 `codex/a5-fds` 验证。macOS 负责编辑/Git/编排，Lima
`criu-dev` 构建 Linux ELF 与模块，嵌套 Linux 5.10.29/aarch64 QEMU 执行
insmod、dump、CRIU 4.2.1 restore。snapshot、镜像、fixture 数据及日志位于
guest-local `/tmp`。使用 guest 内 dmesg 检查 KASAN/lockdep/oops。

成功条件包含恢复后语义检查和第二次命令响应，不能以 restore 返回 0 替代。

## 已验证的 A5 功能

| 门禁 | 结果与覆盖 |
|---|---|
| `a5-cross-restore.sh` | regular、pipe、UNIX 三组真实 restore、行为与继续执行均 PASS |
| regular fixture | independent open / dup offset、mmap 同 inode、高位 fd 1100、1024 个 descriptor、逐 fd CLOEXEC、O_TRUNC 不重放 |
| pipe fixture | 4096 字节内容、部分已读后的剩余字节、dup、恢复后写读、空 pipe 的 EAGAIN、dump 前及恢复后关闭写端的 EOF |
| UNIX fixture | 部分已读的双向队列、dup、恢复后双向通信、半关闭后的 EOF/EPIPE 和反向通信 |
| `a5-unsupported.sh` | 17 个用例均返回 EOPNOTSUPP、不留 snapshot、恢复原任务运行 |
| `a5-images.sh` / `a5-converter-fds.sh` | magic、protobuf 字段、raw framing、共享 ID、CLOEXEC、矛盾/缺失对象图和临时输出清理 PASS |
| `a5-behavior.sh` | Linux 上直接运行三个行为 fixture PASS；macOS 返回 SKIP 77 |
| ABI / fd contracts | `a5-object-record-contract.sh`、`a5-fd-contract.sh` PASS |
| `a5-regression.sh` | 最终构建上 A3 单线程和 A4 九线程真实 restore 均 PASS |
| 其余回归 | converter format/images、A4 thread contract、snapshot writer、task/VMA/page/errors、shim contracts 全部 PASS |

最终 guest 内 dmesg 未出现 BUG/WARNING/Oops、KASAN、lockdep 或 atomic-sleep
错误；QEMU 退出状态为 0。独立只读代码审查报告的发布阻塞项已处理并复审。
Task 1–8 已达到本轮确认范围；代码已快进合并到本地 `main`，并推送远程 `main`。

17 个拒绝用例：fown、deleted、clone-files、thread-files、listener、pathname、
dgram、seqpacket、rights、credentials、passcred、fifo、packet-pipe、lock、
external UNIX peer、external-pipe、shared-pipe。

## 本轮解决的问题

1. **错误 CRIU magic / raw framing。** 对照 upstream proto 与
   `sk-queue.c/pipes.c`，按每条 protobuf 消息后紧跟 raw payload 输出。
2. **只校验 restore 返回码。** 用普通 stdin 文件的追加事件触发检查，随后再要求
   `alive=1`。不依赖尚属 A6 的自定义 signal disposition。
3. **UNIX 已读前缀重复恢复。** Linux 5.10.29 的 stream read 更新
   `UNIXCB(skb).consumed`，不修改 skb->len。采集跳过 consumed，检查两遍长度
   与复制边界。包含部分读取的 fixture 修复前失败、修复后真实 restore PASS。
4. **mmap 与 FD 被错误合并。** VMA backing entry 的 object_id 为零，原先按
   inode/path 误合并显式 FD object。现在对象身份独立；同时复用 FD 中的 inode
   mode，避免 mmap 文件被写死为 0644 后遭 CRIU 拒绝。
5. **描述符 flags 丢失。** 保持 576-byte ABI，尾部历史字段 `object_flags`
   明确定义为每 descriptor 的 flags（bit 0 = CLOEXEC）；共享状态比较排除此字段。
6. **不完整对象图被接受。** 拒绝缺少 pipe data/socket queue、孤立 socket、
   非对称/self peer、方向与 flags 冲突、共享 ID 元数据冲突以及 inode 截断/冲突。
7. **未支持状态静默丢失。** 显式拒绝 ancillary credentials、文件锁、fown、
   外部 IPC 参与者和不在冻结线程组中的 CLONE_FILES owner；同组线程独立 fd
   表也拒绝，避免 converter 误建一个共享 files_id。
8. **镜像输出非事务性。** 写同级临时目录，全部成功后 rename；既有非空镜像
   目录保持不变，失败仅清理本次 staging。

## 复现

以下命令从 macOS 调入 Lima；不在 Lima 内核直接加载模块：

```sh
limactl shell criu-dev bash -lc '
  cd /Users/yhome/.codex/worktrees/56c1/criu_module
  make -C userspace clean all
  make -C tests/progs all
  make -C kernel_module -j4
  export CRIU_SOURCE=/Users/yhome/workspace/source_code/criu_module/criu
  ./scripts/run-qemu.sh --ci --script tests/a5-unsupported.sh
  ./scripts/run-qemu.sh --ci --script tests/a5-cross-restore.sh
  ./scripts/run-qemu.sh --ci --script tests/a5-regression.sh
'
```

`CRIU_SOURCE` 指向已在 Lima 构建的 upstream checkout；staging 只带 CRIU binary
和 Python image decoder，不提交测试 binaries。

## 后续扩展

listener/pathname、datagram/seqpacket、SCM_RIGHTS/credentials、外部 peer、
跨进程/不同 fd 表、FIFO、文件锁、完整 socket options、TCP/INET、删除文件重建、
fown 信号以及全量 ZDTM/GitHub CI 保留为后续任务。定向门禁的 PASS 不表示这些
能力已实现，也不扩展 A5 的已确认发布范围。
