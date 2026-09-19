# A8 跨进程共享资源设计

**状态：** 设计已批准，实施计划已生成

**目标：** 在 A7 已能冻结并 dump 同一 PID namespace 内 descendant closure 的基础上，正确表达和恢复闭包内跨进程共享的 fd/fdtable/file-object 与共享内存关系，避免“内容恢复了但共享关系丢失”的静默错误。

**采用方案：** 方案 1。A8 分两段推进：

1. **A8.1：跨进程 fd/fdtable/file-object 共享。**
2. **A8.2：共享内存。** 首个范围覆盖匿名 `MAP_SHARED` 与 POSIX shm；SysV shm 作为可行性分支，不阻塞核心门禁。

**相关文档：**

- [A8 步骤说明](../steps/A8-shared-resources.md)
- [fd 与共享对象原理](../principles/07-fd-and-shared-objects.md)
- [地址空间与 VMA 原理](../principles/03-memory-and-vma.md)
- [VMA 语义与属性](../principles/10-vma-semantics-and-attributes.md)
- [A5 文件描述符设计](2026-09-16-a5-fds-design.md)
- [A7 进程树设计](2026-09-19-a7-pstree-design.md)
- [A3 问题与解决方法复盘](../A3-问题与解决方法复盘.md)
- CRIU `images/core.proto`、`images/fdinfo.proto`、`images/vma.proto`、`images/pagemap.proto`
- CRIU `criu/files.c`、`criu/pstree.c`、`criu/shmem.c`、`criu/mem.c`

## 1. 核心判断

A8 不是“多 dump 几份资源”，而是把 A5 的对象去重模型从单进程扩展到 A7 的整个 frozen closure。

当前代码已有三类基础：

- A5 已用 `struct file *`、pipe、socket 对象 ID 描述单进程 fd 表内的共享；
- A7 已提供同一 PID namespace 内的多进程 frozen closure；
- converter 已能生成 `pstree.img`、每进程 `ids-$pid.img`、`fdinfo-$id.img`、`files.img` 等 CRIU image。

但当前 A5/A7 合成后的缺口是：对象 map 的生命周期仍偏向“每进程一份”，`ids-$pid.img` 中 `vm_id/files_id/fs_id/sighand_id` 仍由 converter 按 pid 或常数推导。只要两个进程共享同一个 `files_struct` 或同一个 `struct file`，这种局部推导就可能破坏共享关系。

A8 的原则是：

```text
内核 dump 侧负责识别真实对象身份；
snapshot ABI 负责把对象身份传给 converter；
converter 只负责把这些身份映射成 CRIU protobuf/image；
真实 CRIU restore 负责根据 image 重建共享关系。
```

内核仍不直接生产 protobuf。

## 2. A8.1：跨进程 fd/fdtable/file-object

### 2.1 必须支持

- A7 closure 内父子进程通过 `fork()` 继承的同一个 `struct file`；
- 多个进程不同 fd 号引用同一个 `struct file`，恢复后 file offset 共享；
- 多个进程引用同一 pipe 或 connected UNIX stream socket 的两端；
- `CLONE_FILES` 共享同一张 `files_struct` 的进程，恢复后 `ids-$pid.img` 中 `files_id` 相同；
- `fdinfo-$files_id.img` 只按 fd table ID 输出一次；
- `files.img`、`reg-files.img`、`pipes-data.img`、`unixsk.img`、`sk-queues.img` 中对象只描述一次；
- 外部参与者明确拒绝，不产生部分镜像。

### 2.2 明确拒绝

- A7 closure 外仍持有同一 `files_struct`、pipe、socket 或共享 object 的任务；
- A5 尚未支持的 fd 类型：TCP/INET、UNIX datagram/seqpacket、listener、pathname-bound socket、SCM_RIGHTS、FIFO、文件锁、ghost file 等；
- 同一 thread group 内不同线程使用不同 `files_struct` 的复杂场景；
- 需要跨 mount/user/net namespace 解释 socket/path 的对象；
- fown/O_ASYNC 与信号目标联动语义，继续留在 A5/A6 扩展清单中。

### 2.3 `files_id` 与 CRIU restore 约束

CRIU restore 的关键约束：

- `criu/files.c` 用 `item->ids->files_id` 打开 `fdinfo-%u.img`；
- `criu/pstree.c` 在子进程与父进程 `files_id` 相同时设置 `CLONE_FILES`；
- root task 不能因为 `files_id` 与 inventory/root ids 冲突被误判成 image corruption。

因此 A8.1 的最小正确模型是：

```text
struct files_struct *  -> files_id
struct file * / sock * -> file object id
fd number             -> fdinfo entry in fdinfo-$files_id.img
```

共享 fdtable 和共享 file-object 是两层关系，不能混淆：

| 共享层 | 镜像表达 | 行为验证 |
|---|---|---|
| 同一 `files_struct` | 多个 task 的 `files_id` 相同，读同一个 `fdinfo-$files_id.img` | 一方打开/关闭/dup 后另一方 fd 表可见 |
| 同一 `struct file` | 多个 `fdinfo_entry.id` 相同 | 一方读写推进 offset，另一方 offset 同步变化 |
| 同一 inode | 不代表同一打开对象 | 两次 open 同一路径 offset 独立 |

A8 不得用路径相等、设备号/inode 相等替代 `struct file *` 身份。

## 3. A8.2：共享内存

### 3.1 必须支持

- 匿名 `MAP_SHARED|MAP_ANONYMOUS`；
- POSIX shm / tmpfs / memfd 风格的 shmem-backed 映射，前提是能稳定识别同一 `inode`；
- 同一 shmem object 在多个进程不同虚拟地址处映射；
- 同一 shmem object 的部分映射；
- shmem 内容只 dump 一份；
- VMA 地址仍按每进程保存，`shmid` 表示共享对象身份。

### 3.2 明确拒绝或作为可行性分支

- SysV shm 首个 A8 gate 不强制通过；若 Linux 5.10.29 外置模块无法安全访问必要符号或 restore 映射风险过大，返回 `-EOPNOTSUPP` 并记录证据；
- 跨 IPC namespace 的 shm；
- hugepage shm；
- 需要额外权限、LSM、mount namespace 或删除后路径重建的复杂 file-backed shared mappings；
- `CLONE_VM` 但非 `CLONE_THREAD` 的独立进程。

### 3.3 CRIU shmem image 路径校正

当前 CRIU 树中没有 `criu/images/shmem.proto`。匿名/普通 shmem 主要通过以下路径表达：

- `criu/images/vma.proto` 的 `vma_entry.shmid`；
- `criu/images/pagemap.proto` 的 pagemap/page payload；
- `criu/image-desc.c` 中的 `pagemap-shmem-%lu`；
- `criu/shmem.c` 中的 `collect_shmem()`、`add_shmem_area()`、`dump_one_shmem()`、`restore_shmem_content()`。

因此 A8.2 不设计单独的 `shmem.proto` 镜像，而是让 converter 输出：

```text
mm-$pid.img:
  vma_entry.shmid = global shmem object id

pagemap-shmem-$shmid.img:
  shared object contents, emitted once
```

普通 process-private pages 继续使用 `pagemap-$pid.img` / `pages-$pages_id.img`。

## 4. 不纳入 A8 首个 gate 的共享对象

### 4.1 `CLONE_FS`

CRIU 的 `fs-%u.img` restore 路径当前按 pid 打开，而不是按 `fs_id` 打开；`fs_id` 主要参与 task kobj identity 和 clone mask。A8 首个 gate 不把跨进程 `CLONE_FS` 作为完成标准。

如果发现 closure 内共享 `fs_struct`：

- 若 restore 路径不能证明可正确处理，先返回 `-EOPNOTSUPP`；
- 不得只把 `ids.fs_id` 写相同但仍生成多份互相矛盾的 `fs-$pid.img`。

### 4.2 `CLONE_VM` 但非线程

CRIU restore 在 `cr-restore.c` 中对非线程进程的 `CLONE_VM` 路径有 `BUG_ON(ca.clone_flags & CLONE_VM)`。A8 首个 gate 继续拒绝跨进程共享 `mm_struct` 的独立进程。

线程共享地址空间已经由 A4 的 thread-group 模型处理，不属于这里的非线程 `CLONE_VM`。

### 4.3 `sighand_struct`

跨进程共享 sighand 但非线程的场景不纳入 A8 首个 gate。A6 已处理 thread group 内共享 signal handler；A8 不把非线程 `CLONE_SIGHAND` 扩展为完成标准。

## 5. Snapshot ABI 设计

### 5.1 新增 task object ids record

A8 需要让内核成为对象身份来源。新增一个 process-scoped record：

```c
#define CRIU_SNAPSHOT_REC_TASK_IDS 20
#define CRIU_SNAPSHOT_TASK_IDS_VERSION 1U
#define CRIU_SNAPSHOT_TASK_IDS_RECORD_SIZE 32U

struct criu_snapshot_task_ids_record {
	uint32_t version;
	uint32_t pid;
	uint32_t vm_id;
	uint32_t files_id;
	uint32_t fs_id;
	uint32_t sighand_id;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed));
```

字段语义：

- `pid` 是当前 PID namespace 中的 process leader pid；
- `vm_id`、`files_id`、`fs_id`、`sighand_id` 是 dump transaction 内稳定对象 ID；
- A8.1 只依赖 `files_id`；
- `vm_id/fs_id/sighand_id` 仍写入真实去重值，但首个 gate 对 unsupported 共享关系拒绝；
- 新 converter 必须要求 A7/A8 snapshot 中每个 process 都有 task ids record。

### 5.2 FD records 的作用域变化

现有 `CRIU_SNAPSHOT_REC_FD` 保持 576-byte 兼容格式，但在 A8 snapshot 中必须 process-scoped；owner pid 表示“这个 fd binding 属于哪个进程的 fd table view”。

converter 需要按 `files_id` 分组：

```text
process pid -> task_ids.files_id -> fd bindings
```

同一个 `files_id` 只能生成一个 `fdinfo-$files_id.img`。如果两个 process 声称同一个 `files_id` 但 fd binding 不一致，converter 返回 format/inconsistent，不能任选一份。

### 5.3 新增 shared memory records

A8.2 新增 shmem object 与 shmem page run records，保持内核 snapshot 结构化、userspace 生成 CRIU image：

```c
#define CRIU_SNAPSHOT_REC_SHMEM_OBJECT 21
#define CRIU_SNAPSHOT_REC_SHMEM_PAGE_RUN 22
#define CRIU_SNAPSHOT_SHMEM_OBJECT_VERSION 1U
#define CRIU_SNAPSHOT_SHMEM_PAGE_RUN_VERSION 1U
```

建议 payload：

```c
struct criu_snapshot_shmem_object_record {
	uint32_t version;
	uint32_t shmid;
	uint64_t size;
	uint64_t dev;
	uint64_t ino;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed));

struct criu_snapshot_shmem_page_run_record {
	uint32_t version;
	uint32_t shmid;
	uint64_t page_index;
	uint32_t nr_pages;
	uint32_t data_len;
	/* raw page bytes follow */
} __attribute__((packed));
```

VMA record 需要能引用 `shmid`。若当前 VMA payload 已有保留字段，优先使用保留字段；否则新增 A8 v2 VMA record 或新增 `CRIU_SNAPSHOT_REC_VMA_SHARED`。实施前必须先用 ABI contract 锁定格式，避免让旧 converter 误读。

## 6. 内核 dump context

A8 需要把 A5 当前的 per-process map 提升为 transaction-wide context：

```c
struct criu_dump_shared_ctx {
	struct criu_objmap *mm_ids;
	struct criu_objmap *files_ids;
	struct criu_objmap *fs_ids;
	struct criu_objmap *sighand_ids;
	struct criu_objmap *file_objects;
	struct criu_objmap *emitted_file_objects;
	struct criu_objmap *pipes;
	struct criu_objmap *unix_sockets;
	struct criu_objmap *shmem_ids;
	struct criu_objmap *emitted_shmem;
};
```

实际结构可以拆分，但生命周期必须是：

```text
freeze settled
  -> allocate shared dump context
  -> validate whole closure sharing scope
  -> emit task ids for all processes
  -> emit per-process/process-scoped resources using same context
  -> finish/abort writer
  -> free context
  -> thaw
```

不得在每个 process collector 内重新创建 `file_objects`、`emitted`、`pipes`。

## 7. 闭包验证

A8 的正确性依赖“共享对象的所有参与者都在 closure 内”。保守规则如下：

- `files_struct->count` 必须只由 closure 内 task 和 dump pin 引用解释得通；
- pipe readers/writers 必须与 closure 内已知 endpoint 匹配；
- UNIX stream socket peer 必须在 closure 内，peer 关系对称；
- `struct file` 引用计数出现额外 holder 时，除 regular file 外默认拒绝；
- shmem `inode` 如果无法证明所有 mapping owner 在 closure 内，首个 gate 可以拒绝；
- SysV shm 需要额外保存 IPC shm 元数据、权限、attach chunk 顺序和 IPC namespace
  关系；A8 首个实现将其作为 feasibility branch，若无法完整建模则明确
  `-EOPNOTSUPP`，不复用匿名 shmem 的简化 inode/content-only 路径；
- freeze 后 revalidation 发现成员、fdtable、VMA、shmem object 或 refcount 关系变化时整体 abort。

这延续 A5 的保守策略：明确拒绝优于生成会丢共享关系的镜像。

## 8. Converter 设计

converter 新增三阶段模型：

1. **Parse/validate snapshot：** 读取 A7 process graph、A8 task ids、process-scoped FD/VMA/shmem records。
2. **Build global object graph：** 建立 `files_id -> fd table`、`file object id -> CRIU file_entry`、`shmid -> shmem object/pages`。
3. **Emit CRIU images：** 先全局 images，再每 process/thread images。

关键输出：

```text
ids-$pid.img:
  vm_id/files_id/fs_id/sighand_id from TASK_IDS

fdinfo-$files_id.img:
  one per unique files_id

files.img / reg-files.img / pipes-data.img / unixsk.img / sk-queues.img:
  one object graph for whole closure

mm-$pid.img:
  per-process VMA addresses, shared VMA has same shmid

pagemap-shmem-$shmid.img:
  one per shmem object
```

特别注意：`fs-$pid.img` 与 `mm-$pid.img` 仍按 pid 输出。A8 首个 gate 不把相同 `fs_id` 或非线程相同 `vm_id` 当作已支持能力。

## 9. 测试与验收

### 9.1 Host/Lima contract

- ABI packed-size 和 reader malformed fixture；
- objmap transaction-wide scope contract；
- converter `files_id` grouping fixture；
- converter shmem `shmid` grouping fixture；
- unsupported sharing 明确返回 unsupported，不输出部分 image。

### 9.2 Linux 5.10.29 QEMU guest gate

必须使用 A3 复盘中固定的环境职责：

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/a8-cross-restore.sh'
```

门禁不能只看 `criu restore` 返回值，必须验证：

- restored PID 存活；
- restored process tree 仍符合 A7 预期；
- fd offset 共享行为仍成立；
- `CLONE_FILES` fdtable 共享行为仍成立；
- pipe/socket 跨进程通信行为仍成立；
- shared memory 双向可见；
- shmem 内容没有被 dump 多份；
- guest dmesg 干净；
- dump/restore 路径使用 guest local `/tmp` staging，避免 Lima 9p 权限/路径问题。

## 10. 完成标准

A8 首个发布按核心门禁判断：

- A8.1 fd/fdtable/file-object 跨进程共享通过真实 guest cross-restore；
- A8.2 匿名 `MAP_SHARED` / POSIX shm 至少一个核心 shared-memory fixture 通过真实 guest cross-restore；
- SysV shm 要么通过，要么有明确 feasibility 记录和 `-EOPNOTSUPP` 行为；
- 所有 unsupported 项都有明确拒绝路径；
- A3-A7 核心回归仍通过；
- 文档更新支持矩阵与后续扩展清单；
- 不把全量 ZDTM 或 GitHub CI 作为本阶段完成阻塞，但相关失败必须分类记录。
