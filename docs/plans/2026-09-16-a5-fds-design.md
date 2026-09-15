# A5 文件描述符 Dump 设计

## 目标

在 Linux 5.10.29/aarch64 的冻结进程上，采集完整 fd 表，区分独立 `struct file`
与 `dup` 共享对象，并为 CRIU converter 提供可验证的常规文件镜像数据。旧的
A3 560-byte FD 记录继续可读；A5 记录在固定路径字段后追加对象 ID、类型和
能力标志，避免破坏已有 fixture。

## 范围与边界

- 支持 regular file：路径、清理后的 open flags、位置、设备号/inode、大小、fown
  所需的现有凭据数据，以及重复对象的稳定 image id。
- 支持 fd 表中的任意 fd 号和空洞；同一 `struct file *` 只分配一个 object id。
- 识别 FIFO/pipe 和 UNIX socket。当前版本对 pipe 内容、socket 状态、in-flight
  `SCM_RIGHTS` 明确返回 `-EOPNOTSUPP`，不得生成部分镜像。
- 删除文件、跨 mount namespace、文件锁和 TCP socket 同样走明确拒绝路径。
- A3 的 fd 0/1/2 常规文件 fixture 保持原有格式和输出。

## 数据流

1. A2 成功后，在 `files_struct->file_lock` 下复制非空 fd 的 `struct file *` 并
   `get_file()`；释放锁后做路径和 inode 读取，所有引用在函数结束释放。
2. `criu_objmap` 以指针身份映射 object id。fd 记录总是写 object id；第一次
   看到对象时才写完整对象元数据。
3. converter 收集扩展 FD 记录，拒绝重复 fd、重复 object id 的冲突元数据和
   unsupported type；regular file 按 object id 生成一次 `reg-files.img`/`files.img`，
   每个进程生成一个 fdinfo stream。
4. flags 在 dump 侧清理 `O_CREAT|O_EXCL|O_TRUNC`，防止 restore 重新打开时创建、
   截断或覆盖用户文件。

## 失败策略

- 目标消失、fd 引用竞争、路径过长或 ABI 不完整返回错误并 abort 临时 snapshot。
- 类型超出支持矩阵返回 `-EOPNOTSUPP`/`SNAPSHOT_READER_UNSUPPORTED`，不落盘可被
  restore 误读的镜像。
- object map 分配失败、写入失败和重复元数据返回 `-ENOMEM`/I/O 或格式错误。

## 验证门禁

- 先运行 `tests/a5-fd-contract.sh` 的失败测试，覆盖 ABI、objmap、flags 清理、
  非 RCU I/O 和 unsupported 分支。
- converter fixture 覆盖两个独立 open、一个 dup、fd 空洞、regular file 记录和
  pipe/socket 拒绝。
- `make -C userspace`、A3/A4 用户态测试必须保持通过。
- 权威内核验证只在 Lima 构建并由嵌套 Linux 5.10.29 QEMU 执行；macOS/Lima
  5.15 结果不计入完成声明。

## 非目标

本阶段不实现 pipe buffer 内容恢复、UNIX socket 连接/队列恢复、SCM_RIGHTS 图遍历、
文件锁恢复或跨进程共享对象去重；这些保留到 A8/A10，并在步骤文档中记录。
