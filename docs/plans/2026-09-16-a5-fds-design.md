# A5 文件描述符 Dump 设计

## 目标

在冻结的 Linux 5.10.29/aarch64 进程上采集文件描述符对象，并转换为真实 CRIU
可消费的镜像。A5 采用方案 2：完成 regular file、pipe 和已连接的 UNIX stream
socket；对象共享关系必须和数据内容一起保留。

旧的 A3 560-byte FD 记录和当前 A5 576-byte regular-file 记录继续可读。新增
对象专用记录表达 pipe、UNIX socket 及其内容，避免改变既有 ABI 的含义。

## 范围

### 必须支持

- regular file：路径、清理后的 open flags、位置、设备号/inode、大小和对象 ID；
- 任意 fd 号和 fd 空洞；相同 `struct file *` 只建立一个 object ID；
- pipe：两个 endpoint 的关联、读写方向、容量、未读普通字节、空 pipe 和写端
  已关闭状态；
- `AF_UNIX/SOCK_STREAM`：`socketpair()` 或已连接 socket、peer 关系、未读普通字节
  和 shutdown 状态；
- converter 两阶段校验，以及 Linux 5.10.29 guest 中真实 CRIU restore 验证。

### 明确拒绝

以下情况必须返回 `-EOPNOTSUPP` 或 converter unsupported，不能生成部分镜像：

- listener、pathname-bound socket、`AF_UNIX/SOCK_DGRAM`、`SOCK_SEQPACKET`；
- `SCM_RIGHTS`、in-flight fd 和其他 ancillary data；
- 外部 peer、跨 mount namespace socket 路径、TCP/INET socket；
- FIFO、文件锁、packetized pipe 和无法安全复制的 pipe buffer 类型。

## 对象模型

```text
fd -> struct file -> object_id -> type-specific metadata

pipe endpoints -> pipe_id -> shared pipe ring/data
unix socket A <-> peer socket B
```

`object_id` 表示 `struct file *` 的共享；`pipe_id` 表示两个不同 endpoint 背后的
同一个 pipe；UNIX socket 用互相引用的 peer ID 表示连接关系。所有对象在 dump 期间
都必须持有引用，不能把地址复用或路径相等误当成共享关系。

## Snapshot 数据流

1. A2 freezer 成功后，在 `files_struct->file_lock` 下复制 fd 指针并 `get_file()`。
2. 释放锁后执行路径、inode、pipe 和 socket 状态采集；锁内不得睡眠或写镜像。
3. 以指针身份建立 regular-file/endpoint object ID；pipe 和 socket 额外建立关系 ID。
4. pipe ring 和 socket receive queue 只读复制，不调用会消费状态的 `read/recv`。
5. 所有记录先写入临时 snapshot；任一对象失败则整体 abort 并清理。

## CRIU 镜像映射

```text
regular file: files.img + reg-files.img + fdinfo-*.img
pipe:         files.img + pipes-data.img + fdinfo-*.img
unix stream:  files.img + unixsk.img + sk-queues.img + fdinfo-*.img
```

Converter 第一阶段建立 fd bindings、file objects、pipe objects、UNIX socket objects
和数据队列，校验 ID 唯一性、引用完整性、peer 对称性和 raw data 长度。第二阶段才
生成最终 CRIU protobuf 镜像；失败时删除临时输出目录。

恢复顺序由 CRIU 执行：先创建对象和 peer/pipe 关系，再恢复队列内容和 shutdown 状态，
最后以两遍 fd 安装把对象放到目标 fd 号，避免 dup 或服务 fd 冲突。

## 内核采集约束

- regular file 清理 `O_CREAT|O_EXCL|O_TRUNC`，避免 restore 截断或覆盖原文件；
- pipe 通过 5.10.29 的 pipe ring 在内部锁保护下复制有效 buffer，不推进 head/tail；
- UNIX stream 只复制普通 receive queue 字节；发现 ancillary data 立即拒绝；
- pipe endpoint 不能按 `struct file *` 合并，必须通过 pipe inode/pipe object 关联；
- socket peer 必须存在且关系对称；状态超出已连接 stream 矩阵立即拒绝。

## 验证门禁

- ABI、object map、锁边界和错误路径 contract；
- regular file 的 independent open、dup、fd 空洞和 `O_TRUNC`；
- pipe 的两端、内容、空 pipe、EOF；
- UNIX stream socketpair、双向通信、未读数据和 shutdown；
- unsupported 类型拒绝且不留下部分镜像；
- userspace build、A3/A4 回归、shell syntax、`git diff --check`；
- 权威验证只在 Lima 构建并由嵌套 Linux 5.10.29 QEMU 执行模块和 CRIU restore。

macOS 和 Lima 宿主机内核结果不能作为目标 guest 通过证据。dump、镜像和日志固定
使用 guest 本地 `/tmp`，避免 Lima 9p 权限和路径问题。guest 缺少 CRIU 时只能报告
snapshot/converter gate，不能声称 cross-restore 通过。

## 后续扩展备忘

pathname-bound/listen/backlog、UNIX datagram/seqpacket、`SCM_RIGHTS` 对象图、其他
ancillary data、外部 peer、跨 namespace、TCP/INET、FIFO、文件锁、完整 socket options
和多进程共享 fd 表均保留为后续任务，不加入 A5 完成标准。

## A3 复盘约束

开发和验证必须遵循 [A3-问题与解决方法复盘](../A3-问题与解决方法复盘.md)：macOS
只负责编辑/Git/编排，Lima 负责 Linux 构建，嵌套 Linux 5.10.29 guest 负责模块加载和
真实验证；不把环境缺失、权限问题或功能边界误报为实现通过；所有完成声明必须附带
新鲜、可复现的命令和输出。
