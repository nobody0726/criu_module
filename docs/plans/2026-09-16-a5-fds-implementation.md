# A5 文件描述符 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 完成 regular file、pipe 和已连接 UNIX stream socket 的内核 dump、CRIU 镜像转换及 Linux 5.10.29 guest restore 验证。

**Architecture:** 保留 560/576-byte FD ABI，新增 type-specific pipe/socket snapshot records。内核在冻结后先 pin fd，再在锁外只读采集对象状态；converter 先构建并校验完整对象图，再生成 `files.img`、`pipes-data.img`、`unixsk.img` 和 `sk-queues.img`。恢复交给真实 CRIU，测试验证共享关系和数据行为。

**Tech Stack:** Linux 5.10.29/aarch64 kernel module, C11 converter, CRIU protobuf-c images, Lima `criu-dev`, nested QEMU guest, shell/C fixtures.

---

## 执行约束

- 代码和 Git 操作在 macOS A5 worktree；Linux userspace build 在 Lima `criu-dev`；模块加载、目标内核行为和真实 restore 只在嵌套 Linux 5.10.29 guest。
- dump、snapshot、CRIU images 和日志使用 guest-local `/tmp`，不得把 Lima 9p 根目录权限问题当成实现失败。
- 每个阶段先有失败 contract/fixture，再实现最小代码；每项完成后单独提交。
- 任何 unsupported 类型都必须在 dump 或 converter 阶段明确失败，并清理临时输出；不得留下可被 restore 误读的部分镜像。
- guest 缺少 `criu` binary 时，只报告 snapshot/converter gate，不报告 cross-restore PASS。
- 参考 `docs/A3-问题与解决方法复盘.md`，不修改未相关的 A3/A4 行为和生成 artifacts。

## Task 1: 扩展 snapshot ABI 和模型

**Files:**
- Modify: `include/criu_snapshot.h`
- Modify: `kernel_module/checkpoint/dump_files.h`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Create: `tests/a5-object-record-contract.sh`

1. 写失败 contract，要求新增 pipe endpoint、pipe data、UNIX socket 和 socket queue record 常量、固定 packed size、type/object references。
2. 运行 `sh tests/a5-object-record-contract.sh`，确认在实现前失败。
3. 为 snapshot 增加版本化的 type-specific records；保留旧记录可读，所有整数继续 little-endian packed ABI。
4. converter 解析新记录但只构建内存对象表，不立即写镜像；检测短记录、重复 ID、类型冲突和越界长度。
5. 运行 `sh tests/a5-object-record-contract.sh tests/a5-fd-contract.sh`，预期 PASS。
6. 提交：`git commit -m 'feat: extend A5 fd object snapshot ABI'`。

## Task 2: 增加失败 fixture 和对象图校验

**Files:**
- Modify: `tests/a5-converter-fds.sh`
- Create: `tests/progs/fds-pipe.c`
- Create: `tests/progs/fds-unix-stream.c`
- Modify: `tests/progs/Makefile`

1. 为 converter 增加 malformed fixtures：pipe 两端 ID 冲突、socket peer 不对称、queue raw length 错误、ancillary/SCM_RIGHTS 标记和 unsupported socket type。
2. 运行 `sh tests/a5-converter-fds.sh`，确认新增用例先失败。
3. 创建 pipe fixture：保留读端和写端、填入普通未读字节、覆盖 empty pipe 和写端关闭状态。
4. 创建 UNIX stream fixture：`socketpair(AF_UNIX, SOCK_STREAM)`，写入双向未读字节，覆盖 shutdown。
5. converter 先完整校验对象图，再写临时 image directory；失败删除目录。
6. 运行 `sh tests/a5-converter-fds.sh` 和 fixture 的静态构建；提交：`git commit -m 'test: add A5 pipe and unix stream fixtures'`。

## Task 3: 实现 pipe 内核采集

**Files:**
- Create: `kernel_module/checkpoint/dump_pipe.c`
- Create: `kernel_module/checkpoint/dump_pipe.h`
- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `kernel_module/Makefile`
- Modify: `tests/a5-fd-contract.sh`

1. 在 contract 中锁定 Linux 5.10.29 的 pipe 结构访问、endpoint direction、pipe_id、head/tail 只读要求和不支持 buffer 类型错误路径。
2. 运行 contract，确认未实现时失败。
3. 按 inode/pipe object 建立 pipe_id，不能按 endpoint `struct file *` 合并；持有 pipe/file 引用直到 record 写完。
4. 在 pipe 内部锁保护下复制有效 ring buffer，不能推进 head/tail，不能调用 `read()`/`tee()`；记录容量、bytes、方向和 write-end closed 状态。
5. 对 packetized/无法安全复制的 buffer 返回 `-EOPNOTSUPP`，释放所有引用并 abort snapshot。
6. 在 Lima 中构建 Linux module；只在 QEMU guest 中执行 insmod/dump。
7. 运行 `sh tests/a5-fd-contract.sh`；提交：`git commit -m 'feat: dump pipe endpoints and data'`。

## Task 4: 实现 UNIX stream 内核采集

**Files:**
- Create: `kernel_module/checkpoint/dump_unixsk.c`
- Create: `kernel_module/checkpoint/dump_unixsk.h`
- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `kernel_module/Makefile`
- Modify: `tests/a5-fd-contract.sh`

1. 增加失败 contract：listener/pathname/dgram/seqpacket/external peer/ancillary data 必须返回 unsupported。
2. 运行 contract，确认未实现时失败。
3. 识别 `AF_UNIX/SOCK_STREAM` 已连接 socket，记录 socket object、peer object、state、flags、shutdown 和必要 options。
4. 在 socket 锁保护下只读复制普通 receive queue；检测 ancillary data，尤其 `SCM_RIGHTS`，立即失败，不消费队列。
5. 验证 peer 关系对称；任何缺失或 dangling peer 返回 `-EOPNOTSUPP`。
6. 在 Lima 构建并在 Linux 5.10.29 guest 中验证 dump；提交：`git commit -m 'feat: dump connected unix stream sockets'`。

## Task 5: 生成 CRIU pipe/socket images

**Files:**
- Modify: `userspace/criu-module-convert/criu_model.c`
- Modify: `userspace/criu-module-convert/image_writer.c`
- Modify: `tests/a5-converter-fds.sh`
- Create: `tests/a5-images.sh`

1. 为 `files.img` 增加 PIPE/UNIXSK tagged entries；为 pipe 生成 `pipes-data.img`，为 UNIX queue 生成 `unixsk.img` 与 `sk-queues.img`，并保留 `fdinfo` 的 exact fd/object mapping。
2. 使用 CRIU protobuf wire layout，参考 `criu/images/pipe.proto`, `pipe-data.proto`, `sk-unix.proto`, `sk-queue` 相关实现；不得自定义无法被 CRIU 读取的字段。
3. 对每个 object 只生成一次描述；校验 pipe_id、peer、raw data 长度、n_scm==0 和 fd flags。
4. 先写临时目录，所有 images 成功后原子 rename；失败删除临时目录。
5. `tests/a5-images.sh` 解析 image magic 和 protobuf 字段，验证没有重复对象或残留半成品。
6. 在 Lima 构建 converter，运行 `sh tests/a5-converter-fds.sh tests/a5-images.sh`；提交：`git commit -m 'feat: emit CRIU pipe and unix stream images'`。

## Task 6: 用户态行为 fixtures 和 converter gate

**Files:**
- Modify: `tests/progs/fds-pipe.c`
- Modify: `tests/progs/fds-unix-stream.c`
- Create: `tests/a5-behavior.sh`
- Modify: `tests/a5-converter-fds.sh`

1. 为 regular file 验证 independent open 与 dup 的 f_pos 关系。
2. 为 pipe 验证内容字节、两端关联、empty pipe 和写端关闭后的 EOF。
3. 为 UNIX stream 验证 socketpair 双向通信、未读数据、shutdown 和 peer 关系。
4. 为 unsupported cases 验证 listener、pathname-bound、datagram、SCM_RIGHTS、FIFO 明确失败。
5. converter gate 只接受完整对象图；运行 `sh tests/a5-behavior.sh` 和全部 A5 converter tests。
6. 提交：`git commit -m 'test: verify A5 fd restore semantics'`。

## Task 7: Linux 5.10.29 guest gate

**Files:**
- Create: `tests/a5-cross-restore.sh`
- Modify: `docs/steps/A5-fds.md`
- Modify: `docs/03-Iteration-Plan.md`

1. 在 Lima 构建 userspace、fixtures 和 module：

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/.codex/worktrees/56c1/criu_module && \
   make -C userspace && make -C tests/progs fds-dup fds-pipe fds-unix-stream'
```

2. 用 `scripts/run-qemu.sh` 将必要文件 staging 到 guest-local `/tmp`，只在 guest 执行 module load、dump 和 restore。
3. 运行：

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/.codex/worktrees/56c1/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/a5-cross-restore.sh'
```

4. 只有输出同时包含 regular/pipe/UNIX behavior PASS 且 restore 进程存活并继续通信，才记录 `A5_CROSS_RESTORE: PASS`。
5. 如果 guest 没有可用 CRIU，记录 `A5_FD_GUEST: PASS (dump/converter gate; CRIU unavailable)`，并明确 cross-restore 未验证。
6. 检查 guest-local dmesg，确认没有 module warning/oops；不把 Lima host kernel 输出计入目标验证。
7. 提交文档和 gate：`git commit -m 'test: add A5 Linux 5.10.29 guest gate'`。

## Task 8: 回归与发布前检查

**Files:**
- No generated artifacts or logs committed.

1. 在 macOS 运行：

```sh
sh tests/a5-fd-contract.sh
sh tests/a5-converter-fds.sh
sh tests/a5-images.sh
sh tests/a5-behavior.sh
sh -n tests/a5-*.sh
git diff --check
```

2. 在 Lima 运行 `make -C userspace` 和所有静态 fixture build。
3. 在 guest 运行 A3/A4 gates，确认 A5 没有改变已有行为。
4. 运行 `git status --short`，清除测试生成的 `tests/progs/fds-dup`、`fds-pipe`、`fds-unix-stream`，不要提交 `artifacts/`、logs 或 binaries。
5. 使用 `git diff main...HEAD` 审查 ABI、错误清理和环境声明；提交：`git commit -m 'test: complete A5 regression gates'`。

## 完成判定

A5 仅在 Task 1-8 全部完成、真实 guest CRIU restore 行为通过、A3/A4 回归通过且所有延期能力已在文档备忘中列明时发布。缺少 guest CRIU、只通过 converter、或只在 macOS/Lima host kernel 通过，都不能称为 A5 完成。

## 执行记录（2026-09-17）

- 已完成并提交 Task 1 的扩展 ABI/object-record contract：`5e2ed96`。
- 已完成并提交 Task 2 的 pipe/UNIX stream fixtures 和 malformed-object 拒绝用例：`985c533`。
- 已完成 pipe endpoint/unread-data 初步内核采集，修复 Linux 5.10.29 的 `kmap_atomic`
  兼容性，并接入 UNIX stream 元数据/队列采集初版：`1524832`、`d76a8b7`。
- Lima userspace、fixtures 和 Linux 5.10.29 module 构建通过；嵌套 guest 输出
  `A5_FD_GUEST: PASS (dump/converter gate; CRIU unavailable)`。
- macOS 静态链接仍因缺失 `crt0.o` 不可用；该结果不计入 Linux 验证。
- Task 5-8 的完整 CRIU protobuf image 兼容性、行为 fixture 和真实 cross-restore 仍待完成。

## 收尾记录（2026-09-18）

执行依据仍是 Task 1–8；以上 09-17 状态为历史记录。验证证据与复现命令见
[A5 验证记录](2026-09-18-a5-verification.md)。

Task 1–8 已完成：A5 正向真实 restore、17 个拒绝/回滚用例、静态 contracts、
Lima 构建与最终 A3/A4 guest 回归全部通过。全量 ZDTM/GitHub CI 仍为扩展验证，
不作为已执行结果。

- Task 3/4 的 pipe/socket 采集集中在 `checkpoint/dump_files.c`，共享一次 fd
  pin、对象预扫描和错误清理；未拆出计划中的 `dump_pipe.c/dump_unixsk.c`。
- Task 5 实现真实 CRIU magic、protobuf + raw payload、PIPE/UNIXSK tagged entries；
  `tests/a5-images.sh` 复用 converter fixtures，校验完整对象图及原子发布。
- Task 6 使用 stdin 普通文件触发行为检查。不能使用自定义信号处理器，因为其恢复属于
  A6；保持 A3 的默认 disposition 约束。单独保存 descriptor 的 `FD_CLOEXEC`。
- Task 7 验收包含恢复后两次响应，而非只检查 CRIU 返回码。负向用例同时检查
  `EOPNOTSUPP`、snapshot 清理和目标继续运行。
- Task 8 对 A3/A4 进行真实 guest 回归；旧静态 contract 中“仅 fd 0/1/2”和
  “THREAD 是最后一种记录”的断言随 A5 范围更新。
- worktree 不复制 upstream checkout；`CRIU_SOURCE` 让 QEMU staging 复用主仓库
  已构建 CRIU 和 Python decoder。临时 `criu-bin` 不进入提交。
- 实现最初在 `codex/a5-fds` 完成；随后已快进合并到本地 `main` 并推送远程 `main`。
