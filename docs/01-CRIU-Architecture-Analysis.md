# CRIU 项目深度架构分析报告

> 本文档基于 CRIU 源码深入分析，为在 Linux 内核空间实现类似功能提供理论基础。
> 
> 分析日期：2026-08-16  
> CRIU 版本：基于 criu-dev 分支  
> 分析工具：Claude Code + Manual Code Review

---

## 目录

1. [整体架构分析](#1-整体架构分析)
2. [核心数据结构](#2-核心数据结构)
3. [Checkpoint 详细流程](#3-checkpoint-详细流程)
4. [Restore 详细流程](#4-restore-详细流程)
5. [关键技术点](#5-关键技术点)
6. [用户空间 vs 内核空间对比](#6-用户空间-vs-内核空间对比)

---

## 1. 整体架构分析

### 1.1 主要模块划分

CRIU 项目采用模块化设计，主要目录结构：

```
criu/
├── criu/               # 核心工具源码 (93,222 行 C 代码)
├── compel/             # 代码注入框架（独立子项目）
├── images/             # Protobuf 镜像文件定义
├── crit/               # 镜像文件检查工具
├── soccr/              # TCP socket checkpoint/restore 库
├── test/               # 测试套件
│   └── zdtm/          # Zero-Downtime Migration 测试
├── scripts/            # 辅助脚本
└── lib/                # C/Python 库
```

### 1.2 模块职责与相互关系

**核心模块关系图：**

```
                     ┌─────────────────┐
                     │   crtools.c     │
                     │  (main entry)   │
                     └────────┬────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
      │  cr-dump.c   │ │ cr-restore.c │ │ cr-service.c │
      │ (2182 lines) │ │ (3653 lines) │ │   (daemon)   │
      └──────┬───────┘ └──────┬───────┘ └──────────────┘
             │                │
             │                │
      ┌──────▼────────────────▼───────┐
      │                                │
      │  ┌──────────────────────────┐ │
      │  │   Compel Framework       │ │
      │  │  ┌─────────┬──────────┐  │ │
      │  │  │ infect  │ parasite │  │ │
      │  │  │ (注入)   │ (代码块) │  │ │
      │  │  └─────────┴──────────┘  │ │
      │  └──────────────────────────┘ │
      │                                │
      └────────────────────────────────┘
                     │
         ┌───────────┼───────────┐
         ▼           ▼           ▼
    ┌────────┐  ┌────────┐  ┌────────┐
    │ mem.c  │  │files.c │  │ net.c  │
    │(1805行)│  │(1827行)│  │(4184行)│
    └────────┘  └────────┘  └────────┘
         │           │           │
         └───────────┼───────────┘
                     ▼
         ┌────────────────────────┐
         │  Protobuf Image Files  │
         │   (序列化存储格式)      │
         └────────────────────────┘
```

**关键依赖关系：**

1. **criu/** → **compel/**: CRIU 使用 Compel 进行进程注入和控制
2. **criu/** → **images/**: 使用 Protobuf 定义进行序列化
3. **soccr/** ← **criu/sk-tcp.c**: TCP socket 状态的 C/R
4. **crit/** → **images/**: 解析和操作镜像文件

### 1.3 代码组织结构（按功能域）

**主要功能模块代码行数统计：**

| 模块 | 文件 | 行数 | 职责 |
|------|------|------|------|
| 网络 | net.c | 4,184 | 网络命名空间和设备 |
| 挂载 | mount.c | 4,073 | 挂载点处理 |
| 恢复 | cr-restore.c | 3,653 | Restore 主流程 |
| 进程解析 | proc_parse.c | 2,978 | /proc 文件系统解析 |
| PIE Restorer | pie/restorer.c | 2,854 | 恢复时执行的 PIE 代码 |
| 页面映射 | pagemap.c | 2,725 | 页面映射分析 |
| 常规文件 | files-reg.c | 2,635 | 常规文件处理 |
| 页面传输 | page-xfer.c | 2,559 | 页面传输（网络/本地）|
| Unix Socket | sk-unix.c | 2,453 | Unix domain socket |
| TTY | tty.c | 2,452 | 终端处理 |
| Cgroup | cgroup.c | 2,403 | Cgroup 管理 |
| 内核特性 | kerndat.c | 2,251 | 内核特性检测 |
| 工具函数 | util.c | 2,242 | 通用工具函数 |
| Dump | cr-dump.c | 2,182 | Checkpoint 主流程 |
| 命名空间 | namespaces.c | 2,006 | 命名空间管理 |

**按功能域分类：**

```
criu/
├── 进程管理
│   ├── pstree.c          # 进程树构建
│   ├── seize.c           # 进程抓取（ptrace SEIZE）
│   └── clone-noasan.c    # 进程创建
│
├── 内存管理
│   ├── mem.c             # 内存 dump/restore 核心
│   ├── pagemap.c         # 页面映射分析
│   ├── pagemap-cache.c   # 页面映射缓存
│   ├── page-pipe.c       # 零拷贝管道
│   └── page-xfer.c       # 页面传输
│
├── 虚拟内存区域
│   ├── proc_parse.c      # /proc 解析（smaps, maps）
│   ├── vdso.c            # vDSO 处理
│   └── uffd.c            # userfaultfd（懒加载）
│
├── 文件描述符
│   ├── files.c           # FD 框架
│   ├── files-reg.c       # 常规文件
│   ├── pipes.c           # 管道
│   ├── eventpoll.c       # epoll
│   ├── eventfd.c         # eventfd
│   ├── signalfd.c        # signalfd
│   └── timerfd.c         # timerfd
│
├── 网络
│   ├── net.c             # 网络命名空间
│   ├── sk-unix.c         # Unix socket
│   ├── sk-inet.c         # TCP/UDP socket
│   ├── sk-tcp.c          # TCP 连接状态
│   └── sk-packet.c       # Packet socket
│
├── 命名空间
│   ├── namespaces.c      # NS 框架
│   ├── mount.c           # Mount NS
│   ├── ipc_ns.c          # IPC NS
│   ├── uts_ns.c          # UTS NS
│   └── timens.c          # Time NS
│
├── 安全
│   ├── lsm.c             # LSM 框架
│   ├── apparmor.c        # AppArmor
│   ├── seccomp.c         # Seccomp
│   └── creds.c           # 凭证
│
└── PIE（Position Independent Executable）
    └── pie/
        ├── parasite.c    # Parasite 代码（注入到目标进程）
        └── restorer.c    # Restorer 代码（恢复时执行）
```

---

## 2. 核心数据结构

### 2.1 进程树表示（pstree）

**定义位置：** `criu/include/pstree.h`

```c
struct pstree_item {
    struct pstree_item *parent;      // 父进程
    struct list_head children;        // 子进程链表
    struct list_head sibling;         // 兄弟进程链表
    
    struct pid *pid;                  // PID 信息（含命名空间）
    pid_t pgid;                       // 进程组 ID
    pid_t sid;                        // 会话 ID
    pid_t born_sid;                   // 原始会话 ID
    
    int nr_threads;                   // 线程数量
    struct pid *threads;              // 线程数组
    CoreEntry **core;                 // 核心状态（每线程一个）
    TaskKobjIdsEntry *ids;            // 内核对象 ID
    
    union {
        futex_t task_st;              // 任务状态（原子操作）
        unsigned long task_st_le_bits;
    };
};
```

**Dump 时扩展信息（dmpi）：**

```c
struct dmp_info {
    struct ns_id *netns;                    // 网络命名空间
    struct page_pipe *mem_pp;               // 内存页管道
    struct parasite_ctl *parasite_ctl;      // Parasite 控制结构
    struct parasite_thread_ctl **thread_ctls; // 线程控制
    uint64_t *thread_sp;                    // 线程栈指针
    struct thread_lsm **thread_lsms;        // LSM 配置文件
};
```

### 2.2 虚拟内存区域（VMA）

**C 结构定义：** `criu/include/vma.h`

```c
struct vm_area_list {
    struct list_head h;                    // VMA 链表头
    unsigned nr;                           // VMA 总数
    unsigned int nr_aios;                  // AIO VMA 数量
    union {
        unsigned long nr_priv_pages;      // dump: 私有页数量
        unsigned long rst_priv_size;      // restore: 私有区域大小
    };
    unsigned long nr_priv_pages_longest;  // 最长私有 VMA 页数
    unsigned long nr_shared_pages_longest;// 最长共享 VMA 页数
};

struct vma_area {
    struct list_head list;
    VmaEntry *e;                          // Protobuf 条目
    
    union {
        struct /* dump 时使用 */ {
            int vm_socket_id;
            char *aufs_rpath;             // AUFS 根路径
            struct stat *vmst;            // 文件状态
            int mnt_id;                   // 挂载点 ID
        };
        
        struct /* restore 时使用 */ {
            int (*vm_open)(int pid, struct vma_area *vma);
            struct file_desc *vmfd;       // 文件描述符
            struct vma_area *pvma;        // 父 VMA（COW）
            unsigned long *page_bitmap;   // 页面位图
            unsigned long premmaped_addr; // 预映射地址
        };
    };
};
```

**Protobuf 定义：** `images/vma.proto`

```protobuf
message vma_entry {
    required uint64 start   = 1;  // 起始地址
    required uint64 end     = 2;  // 结束地址
    required uint64 pgoff   = 3;  // 页偏移
    required uint64 shmid   = 4;  // 共享内存 ID
    required uint32 prot    = 5;  // 保护标志（PROT_*）
    required uint32 flags   = 6;  // 映射标志（MAP_*）
    required uint32 status  = 7;  // 状态标志
    required sint64 fd      = 8;  // 文件描述符
    optional uint64 madv    = 9;  // madvise 标志
}
```

### 2.3 核心状态（CoreEntry）

**Protobuf 定义：** `images/core.proto`

```protobuf
message core_entry {
    enum march {
        X86_64 = 1;
        ARM = 2;
        AARCH64 = 3;
        PPC64 = 4;
        S390 = 5;
        MIPS = 6;
        LOONGARCH64 = 7;
        RISCV64 = 8;
    }
    
    required march mtype = 1;
    optional thread_info_x86 thread_info = 2;  // 架构特定寄存器
    optional task_core_entry tc = 3;           // 任务核心信息
    optional task_kobj_ids_entry ids = 4;      // 内核对象 ID
    optional thread_core_entry thread_core = 5; // 线程核心信息
}

message task_core_entry {
    required uint32 task_state = 1;      // 任务状态
    required uint32 exit_code = 2;       // 退出码
    required uint32 personality = 3;     // Personality
    required uint32 flags = 4;           // 标志
    required uint64 blk_sigset = 5;      // 阻塞信号集
    required string comm = 6;            // 命令名
    optional task_timers_entry timers = 7;
    optional task_rlimits_entry rlimits = 8;
    repeated sa_entry sigactions = 15;   // 信号处理函数
}

message task_kobj_ids_entry {
    required uint32 vm_id = 1;       // VM 空间 ID
    required uint32 files_id = 2;    // 文件表 ID
    required uint32 fs_id = 3;       // FS 结构 ID
    required uint32 sighand_id = 4;  // 信号处理 ID
    optional uint32 pid_ns_id = 5;   // PID 命名空间 ID
    optional uint32 net_ns_id = 6;   // 网络命名空间 ID
    // ... 其他命名空间 ID
}
```

### 2.4 镜像文件类型

**所有镜像文件类型：** `criu/include/image-desc.h`

共定义了 **100+ 种镜像文件类型**，主要包括：

- `CR_FD_CORE` - 进程核心状态
- `CR_FD_MM` - 内存映射
- `CR_FD_PAGEMAP` - 页面映射
- `CR_FD_PAGES` - 页面内容
- `CR_FD_FDINFO` - 文件描述符
- `CR_FD_PSTREE` - 进程树
- `CR_FD_NETDEV` - 网络设备
- `CR_FD_MNTS` - 挂载点
- `CR_FD_CGROUP` - Cgroup
- `CR_FD_SECCOMP` - Seccomp 过滤器
- ...等等

---

## 3. Checkpoint 详细流程

### 3.1 完整调用链

**入口点：** `crtools.c:main()` → `cr_dump_tasks()`

```
main() [crtools.c:128]
  └─> cr_dump_tasks() [cr-dump.c:1990]
       ├─> init_stats()                    # 初始化统计
       ├─> kerndat_init()                  # 检测内核特性
       ├─> cr_plugin_init()                # 初始化插件
       │
       ├─> collect_pstree()                # ★ 收集进程树
       │    ├─> compel_interrupt_task()    [seize.c:1089]
       │    │    ├─> ptrace(PTRACE_SEIZE, pid, NULL, 0)
       │    │    └─> ptrace(PTRACE_INTERRUPT, pid, NULL, NULL)
       │    │
       │    ├─> compel_wait_task()         # 等待进程停止
       │    │
       │    └─> collect_task()             [seize.c:~900]
       │         └─> 递归收集子进程和线程
       │
       ├─> collect_pstree_ids()            # 收集内核对象 ID
       ├─> collect_namespaces()            # 收集命名空间
       ├─> collect_file_locks()            # 收集文件锁
       │
       └─> for_each_pstree_item():
            └─> dump_one_task() [cr-dump.c:1413]
                 │
                 ├─> parse_pid_stat()               # 解析 /proc/pid/stat
                 ├─> collect_mappings()             # ★ 收集内存映射
                 │    └─> parse_smaps()            [proc_parse.c]
                 │         └─> 解析 /proc/pid/smaps
                 │
                 ├─> collect_fds()                  # 收集文件描述符
                 │    └─> opendir_proc(pid, "fd")
                 │
                 ├─> parasite_infect_seized()       # ★ 注入 Parasite
                 │    └─> compel_infect()          [compel/src/lib/infect.c]
                 │
                 ├─> dump_task_core_all()          # ★ Dump 核心状态
                 │    ├─> dump_thread_core()
                 │    │    ├─> ptrace(PTRACE_GETREGS)  # 获取寄存器
                 │    │    └─> parasite_dump_thread()  # Parasite 获取内部状态
                 │    │
                 │    ├─> dump_task_signals()       # 信号队列
                 │    │    └─> ptrace(PTRACE_PEEKSIGINFO)
                 │    │
                 │    └─> dump_task_rseq()          # rseq 状态
                 │
                 ├─> dump_task_mm()                # ★ Dump 内存
                 │    └─> parasite_dump_pages()    [mem.c]
                 │         └─> vmsplice() 零拷贝
                 │
                 ├─> dump_task_files_seized()      # ★ Dump 文件
                 │    ├─> parasite_drain_fds()     # Parasite 获取 FD 详情
                 │    └─> dump_one_file()          # 根据类型 dump
                 │
                 ├─> dump_task_fs()                # cwd, root
                 ├─> dump_task_ids()               # 内核对象 ID
                 ├─> dump_task_creds()             # 凭证
                 │
                 └─> parasite_cure_seized()        # ★ 移除 Parasite
```

### 3.2 关键步骤详解

#### 3.2.1 进程抓取（Seize）

**位置：** `criu/seize.c:collect_pstree()`

```c
int collect_pstree(void)
{
    pid_t pid = root_item->pid->real;
    
    // 1. 使用 PTRACE_SEIZE 附加到进程
    if (compel_interrupt_task(pid)) {
        set_cr_errno(ESRCH);
        goto err;
    }
    
    // 2. 等待进程停止
    ret = compel_wait_task(pid, -1, parse_pid_status, NULL, &creds.s, NULL);
    
    // 3. 递归收集进程树
    ret = collect_task(root_item);
    
    return 0;
}
```

**ptrace SEIZE vs ATTACH：**

```c
// PTRACE_SEIZE (CRIU 使用)
ptrace(PTRACE_SEIZE, pid, NULL, 0);       // 不发送信号
ptrace(PTRACE_INTERRUPT, pid, NULL, NULL); // 显式中断

// PTRACE_ATTACH (传统方式)
ptrace(PTRACE_ATTACH, pid, NULL, NULL);    // 自动发送 SIGSTOP
```

#### 3.2.2 Compel 注入流程

**位置：** `compel/src/lib/infect.c`

**完整注入流程：**

```
1. Seize 目标进程
   ├─> ptrace(PTRACE_SEIZE)
   └─> ptrace(PTRACE_INTERRUPT)

2. 分配共享内存
   ├─> 找到可执行内存区域
   ├─> 通过 ptrace 注入 mmap() syscall
   │    ├─> 保存原始寄存器
   │    ├─> 设置 RAX = __NR_mmap
   │    ├─> 设置参数 (size=64KB, PROT_RWX)
   │    ├─> PTRACE_SETREGS
   │    ├─> PTRACE_CONT
   │    └─> wait4() 等待 syscall 完成
   └─> 获取映射地址

3. 注入 Parasite Blob
   ├─> 读取 PIE blob (parasite-blob.o)
   ├─> 重定位代码
   └─> PTRACE_POKEDATA 写入共享内存

4. 执行 Parasite
   ├─> 设置参数到共享内存
   ├─> 设置 RIP 指向 parasite 入口
   ├─> PTRACE_CONT
   └─> wait4() 等待完成

5. Cure（清理）
   ├─> munmap 共享内存
   └─> PTRACE_DETACH
```

**关键代码：**

```c
// compel/src/lib/infect.c:compel_execute_syscall()
static int compel_execute_syscall(struct parasite_ctl *ctl,
                                   user_regs_struct_t *regs)
{
    // 保存原始寄存器
    user_regs_struct_t orig_regs;
    ptrace(PTRACE_GETREGS, pid, NULL, &orig_regs);
    
    // 设置系统调用
    ptrace(PTRACE_SETREGS, pid, NULL, regs);
    
    // 执行
    ptrace(PTRACE_CONT, pid, NULL, NULL);
    wait4(pid, &status, __WALL, NULL);
    
    // 恢复寄存器
    ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
    
    return 0;
}
```

#### 3.2.3 Parasite 代码实现

**位置：** `criu/pie/parasite.c`

**主要命令：**

```c
enum {
    PARASITE_CMD_DUMP_THREAD = 1,
    PARASITE_CMD_MPROTECT_VMAS,
    PARASITE_CMD_DUMPPAGES,        // ★ Dump 内存页
    PARASITE_CMD_DUMP_SIGACTS,     // ★ Dump 信号处理
    PARASITE_CMD_DUMP_ITIMERS,     // ★ Dump interval timers
    PARASITE_CMD_DUMP_POSIX_TIMERS,// ★ Dump POSIX timers
    PARASITE_CMD_DUMP_MISC,        // ★ Dump 杂项信息
    PARASITE_CMD_DRAIN_FDS,        // ★ 获取 FD 详情
    PARASITE_CMD_GET_PROC_FD,
    PARASITE_CMD_DUMP_TTY,
    PARASITE_CMD_CHECK_VDSO_MARK,
    PARASITE_CMD_CHECK_AIOS,
    PARASITE_CMD_DUMP_CGROUP,
};
```

**内存 Dump（vmsplice 零拷贝）：**

```c
// parasite.c:dump_pages()
static int dump_pages(struct parasite_dump_pages_args *args)
{
    int p, ret;
    struct iovec *iovs = pargs_iovs(args);
    
    // 1. 从 CRIU 接收管道 FD
    int tsock = parasite_get_rpc_sock();
    p = recv_fd(tsock);
    
    // 2. 使用 vmsplice 将内存页面发送到管道
    ret = sys_vmsplice(p, iovs, nr_segs, 
                      SPLICE_F_GIFT | SPLICE_F_NONBLOCK);
    
    // SPLICE_F_GIFT: 将页面所有权转移给内核
    // 避免了用户空间 → 内核的拷贝
    
    sys_close(p);
    return 0;
}
```

**零拷贝原理：**

```
传统方式：
进程内存 → read() → 内核缓冲区 → write() → 文件
         (拷贝1)              (拷贝2)

vmsplice 方式：
进程内存 → vmsplice() → pipe → splice() → 文件
         (页面引用)          (直接DMA)
共 0 次拷贝，只更新页表
```

#### 3.2.4 内存 Dump 详细流程

**位置：** `criu/mem.c:parasite_dump_pages_seized()`

```c
int parasite_dump_pages_seized(struct pstree_item *item,
                               struct vm_area_list *vmas,
                               struct mem_dump_ctl *mdc,
                               struct parasite_ctl *ctl)
{
    // 1. 创建 page-pipe（零拷贝管道数组）
    struct page_pipe *pp = create_page_pipe();
    
    // 2. 遍历 VMA，决定哪些页面需要 dump
    for_each_vma(vma, vmas) {
        // 读取 /proc/pid/pagemap
        pmc_t pmc = { .fd = pagemap_fd };
        
        for (addr = vma->start; addr < vma->end; addr += PAGE_SIZE) {
            u64 pme = pmc.map[PAGE_PFN(addr)];
            
            // 检查页面状态
            if (pme & PME_PRESENT) {
                if (pme & PME_SOFT_DIRTY || !opts.track_mem) {
                    // 页面需要 dump
                    add_iov_to_pipe(pp, addr, PAGE_SIZE);
                }
            }
        }
    }
    
    // 3. 通过 Parasite 执行 vmsplice
    ret = parasite_dump_pages(ctl, pp, vmas);
    
    // 4. CRIU 从管道读取并写入镜像
    ret = page_xfer_dump_pages(pp, img_fd);
    
    return 0;
}
```

**增量 Dump（Soft-Dirty Tracking）：**

```c
// 启用 soft-dirty 跟踪
int fd = open("/proc/PID/clear_refs", O_WRONLY);
write(fd, "4", 1);  // 清除 soft-dirty 位

// 之后，访问过的页面会设置 PME_SOFT_DIRTY 位
// 第二次 dump 时只需 dump 脏页
```

#### 3.2.5 文件描述符 Dump

**位置：** `criu/files.c:dump_task_files_seized()`

```c
int dump_task_files_seized(struct parasite_ctl *ctl,
                           struct pstree_item *item,
                           struct parasite_drain_fd *dfds)
{
    // 1. 通过 Parasite 获取 FD 详细信息
    ret = parasite_drain_fds(ctl, dfds);
    // Parasite 调用：
    //   - fcntl(fd, F_GETFL) 获取标志
    //   - fcntl(fd, F_GETFD) 获取 FD_CLOEXEC
    //   - fcntl(fd, F_GETOWN_EX) 获取所有者
    
    // 2. 遍历所有 FD
    for (i = 0; i < dfds->nr_fds; i++) {
        struct fd_parms *p = &dfds->fds[i];
        
        // 3. 根据文件类型分发到不同的 dump 函数
        struct file_desc *desc = collect_fd(p);
        
        switch (desc->ops->type) {
            case FD_TYPES__REG:
                ret = dump_reg_file(p, fd, img);
                break;
            case FD_TYPES__PIPE:
                ret = dump_one_pipe(p);
                break;
            case FD_TYPES__UNIXSK:
                ret = dump_one_unix_fd(p);
                break;
            case FD_TYPES__INETSK:
                ret = dump_one_inet_fd(p);
                break;
            // ... 其他类型
        }
    }
    
    return 0;
}
```

#### 3.2.6 TCP Socket Dump

**位置：** `criu/sk-tcp.c:dump_one_tcp()`

```c
int dump_one_tcp(int fd, struct inet_sk_desc *sk)
{
    TcpStreamEntry tse = TCP_STREAM_ENTRY__INIT;
    
    // 1. 提取连接 5 元组
    tse.src_addr = sk->src_addr;
    tse.src_port = sk->src_port;
    tse.dst_addr = sk->dst_addr;
    tse.dst_port = sk->dst_port;
    
    // 2. 如果启用 --tcp-established
    if (opts.tcp_established) {
        // 使用 TCP_REPAIR 模式获取内部状态
        int on = 1;
        setsockopt(fd, SOL_TCP, TCP_REPAIR, &on, sizeof(on));
        
        // 获取序列号
        struct tcp_repair_opt opts[4];
        getsockopt(fd, SOL_TCP, TCP_REPAIR_OPTIONS, opts, &len);
        
        // 获取发送/接收队列
        setsockopt(fd, SOL_TCP, TCP_REPAIR_QUEUE, TCP_SEND_QUEUE);
        // 读取发送队列数据...
        
        setsockopt(fd, SOL_TCP, TCP_REPAIR_QUEUE, TCP_RECV_QUEUE);
        // 读取接收队列数据...
        
        // 关闭 REPAIR 模式
        on = 0;
        setsockopt(fd, SOL_TCP, TCP_REPAIR, &on, sizeof(on));
    }
    
    // 3. 写入镜像
    pb_write_one(img, &tse, PB_TCP_STREAM);
    
    return 0;
}
```

---

## 4. Restore 详细流程

### 4.1 完整调用链

**入口点：** `crtools.c:main()` → `cr_restore_tasks()`

```
main() [crtools.c:318]
  └─> cr_restore_tasks() [cr-restore.c:末尾]
       ├─> init_stats()
       ├─> kerndat_init()
       ├─> read_pstree_image()           # 读取进程树镜像
       ├─> prepare_task_entries()        # 准备任务条目
       │    └─> 分配共享 task_entries 结构
       │         ├─> futex start         # 阶段同步
       │         └─> futex nr_in_progress
       │
       ├─> prepare_pstree()              # 准备进程树
       ├─> crtools_prepare_shared()      # 准备共享资源
       │    ├─> prepare_files()          # 准备文件描述符
       │    ├─> collect_inet_sk()        # 收集 socket
       │    └─> tty_prep_fds()
       │
       ├─> prepare_restorer_blob()       # 准备 restorer PIE blob
       │
       └─> restore_root_task()           # 恢复根任务
            └─> restore_task_with_children()
                 └─> __restore_task_with_children()
                      │
                      ├─> CR_STATE_ROOT_TASK (阶段 0)
                      │
                      ├─> prepare_namespace()
                      │    ├─> unshare(CLONE_NEWNS | ...)
                      │    ├─> restore_mnt_ns()
                      │    └─> restore_net_ns()
                      │
                      ├─> CR_STATE_PREPARE_NAMESPACES (阶段 1)
                      │
                      ├─> prepare_mappings()         # 准备内存映射
                      │    ├─> premap_priv_vmas()
                      │    └─> restore_priv_vma_content()
                      │
                      ├─> CR_STATE_FORKING (阶段 2)
                      │
                      ├─> create_children_and_session()
                      │    └─> fork_with_pid()       # 使用 clone3 恢复 PID
                      │         └─> 子进程递归调用
                      │
                      ├─> CR_STATE_PRE_RESTORER (阶段 3)
                      │
                      ├─> restore_one_task()         # 恢复单个任务
                      │    ├─> sigreturn_prep()
                      │    └─> sigreturn_restore()  # 切换到 restorer
                      │
                      └─> Restorer PIE blob 接管
                           │
                           ├─> CR_STATE_RESTORE (阶段 4)
                           │    ├─> restore_thread_common()
                           │    ├─> restore_vmas()
                           │    ├─> restore_posix_timers()
                           │    └─> unmap CRIU 内存
                           │
                           ├─> CR_STATE_RESTORE_SIGCHLD (阶段 5)
                           │
                           ├─> CR_STATE_RESTORE_CREDS (阶段 6)
                           │    ├─> restore_creds()
                           │    ├─> restore_seccomp()
                           │    └─> restore_pdeath_sig()
                           │
                           ├─> CR_STATE_COMPLETE (阶段 7)
                           │
                           └─> rt_sigreturn() # 恢复到原始执行点
```

### 4.2 七个恢复阶段

**阶段定义：** `criu/include/restorer.h`

```c
enum {
    CR_STATE_FAIL = -1,
    CR_STATE_ROOT_TASK = 0,          // 根任务创建
    CR_STATE_PREPARE_NAMESPACES,     // 准备命名空间
    CR_STATE_FORKING,                // Fork 子进程
    CR_STATE_PRE_RESTORER,           // Pre-restorer
    CR_STATE_RESTORE,                // 主恢复阶段
    CR_STATE_RESTORE_SIGCHLD,        // 恢复 SIGCHLD
    CR_STATE_RESTORE_CREDS,          // 恢复凭证
    CR_STATE_COMPLETE                // 完成
};
```

**阶段同步机制（Futex）：**

```c
// Root task 控制阶段切换
static int restore_switch_stage(int next_stage)
{
    // 1. 设置参与者数量
    futex_set(&task_entries->nr_in_progress, 
              stage_participants(next_stage));
    
    // 2. 切换阶段并唤醒所有等待的任务
    futex_set_and_wake(&task_entries->start, next_stage);
    
    // 3. 等待所有参与者完成
    return restore_wait_inprogress_tasks();
}

// 每个任务在阶段完成时调用
#define restore_finish_stage(task_entries, stage) \
    futex_dec_and_wake(&(task_entries)->nr_in_progress); \
    futex_wait_while(&(task_entries)->start, stage);
```

### 4.3 Restorer PIE Blob 机制

**位置：** `criu/pie/restorer.c`

**内存布局：**

```
高地址
┌────────────────────────────────┐
│     CRIU 保留区域              │
│  (task_restore_args, etc.)     │
├────────────────────────────────┤
│     Restorer Code Blob         │
│   (PIE，可重定位)               │
├────────────────────────────────┤
│     Restorer Stack             │
│   (32KB per task)              │
├────────────────────────────────┤
│     Signal Frame               │
│   (用于 sigreturn)             │
├────────────────────────────────┤
│     应用程序原始内存映射        │
│   (VMA: heap, stack, libs...)  │
└────────────────────────────────┘
低地址
```

**Restorer 主流程：**

```c
void cr_restore_rt(void) asm("__cr_restore_rt");

void cr_restore_rt(void)
{
    struct task_restore_args *task_args;
    
    // CR_STATE_RESTORE
    restore_finish_stage(task_entries_local, CR_STATE_RESTORE);
    
    // 1. 如果是线程（非 leader）
    if (my_args->pid != task_args->t->pid) {
        restore_thread_common(my_args);
        goto thread_finish;
    }
    
    // 2. Leader 恢复任务特定资源
    
    // 3. 恢复 VMA 内容
    for (i = 0; i < task_args->vmas_n; i++) {
        VmaEntry *vma = &task_args->vmas[i];
        
        // 映射 VMA
        sys_mmap((void *)vma->start, vma_entry_len(vma),
                 vma->prot, vma->flags, -1, 0);
        
        // 恢复页面内容
        restore_vma_content(vma);
    }
    
    // 4. 恢复定时器
    for (i = 0; i < task_args->posix_timers_n; i++) {
        timer_create(...);
        timer_settime(...);
    }
    
    // 5. 恢复信号
    for (i = 0; i < my_args->siginfo_n; i++) {
        sys_rt_sigqueueinfo(...);
    }
    
    // CR_STATE_RESTORE_SIGCHLD
    restore_finish_stage(task_entries_local, CR_STATE_RESTORE_SIGCHLD);
    sys_sigaction(SIGCHLD, &task_args->sigchld_act, NULL);
    
    // CR_STATE_RESTORE_CREDS
    restore_finish_stage(task_entries_local, CR_STATE_RESTORE_CREDS);
    
    // 6. 恢复凭证（最后，安全考虑）
    restore_creds(my_args->creds_args);
    
    // 7. 恢复 Seccomp
    restore_seccomp(my_args);
    
    // 8. Unmap CRIU 相关内存
    sys_munmap(task_args->bootstrap_start, task_args->bootstrap_len);
    sys_munmap(task_args->rst_mem, task_args->rst_mem_size);
    
    // CR_STATE_COMPLETE
    restore_finish_stage(task_entries_local, CR_STATE_COMPLETE);
    
    // 9. 最终 sigreturn，恢复原始执行点
    ARCH_RT_SIGRETURN();  // 跳转到保存的 RIP
}
```

### 4.4 PID 恢复技术

**方法 1: clone3 + CLONE_SET_TID（Linux 5.5+）**

```c
// fork_with_pid() [cr-restore.c]
struct clone_args {
    u64 flags;
    u64 set_tid;        // 指向 PID 数组
    u64 set_tid_size;   // 支持嵌套 PID NS
    // ... 其他字段
};

pid_t target_pid = 1234;
struct clone_args args = {
    .flags = CLONE_NEWPID | CLONE_VM | ...,
    .set_tid = (u64)&target_pid,
    .set_tid_size = 1,
};

pid = syscall(__NR_clone3, &args, sizeof(args));
// 内核保证分配指定的 PID
```

**方法 2: fork 循环（传统方法）**

```c
// 必须在新的 PID 命名空间中
unshare(CLONE_NEWPID);

// PID 从 1 开始递增分配
pid_t target = 42;
while (getpid() < target - 1) {
    pid = fork();
    if (pid == 0) exit(0);  // 占用一个 PID
    if (pid > 0) waitpid(pid, NULL, 0);
}

// 下一个 fork() 将获得目标 PID
```

---

## 5. 关键技术点

### 5.1 ptrace 使用方式

**CRIU 使用的 ptrace 操作：**

| 操作 | 用途 |
|------|------|
| PTRACE_SEIZE | 附加到进程（不发送信号）|
| PTRACE_INTERRUPT | 中断进程执行 |
| PTRACE_GETREGS | 读取通用寄存器 |
| PTRACE_SETREGS | 写入通用寄存器 |
| PTRACE_GETFPREGS | 读取浮点寄存器 |
| PTRACE_PEEKSIGINFO | 读取信号队列 |
| PTRACE_POKEDATA | 写入内存（注入代码）|
| PTRACE_PEEKDATA | 读取内存 |

**PTRACE_SEIZE vs PTRACE_ATTACH：**

```c
// PTRACE_ATTACH (传统)
ptrace(PTRACE_ATTACH, pid);  // 自动发送 SIGSTOP，进程可见

// PTRACE_SEIZE (CRIU 使用)
ptrace(PTRACE_SEIZE, pid);       // 不影响进程状态
ptrace(PTRACE_INTERRUPT, pid);   // 显式中断，对进程透明
```

### 5.2 vmsplice/splice 零拷贝

**传统内存 dump：**
```
用户空间 → read() → 内核缓冲区 → write() → 磁盘
         (拷贝1)              (拷贝2)
```

**vmsplice 零拷贝：**
```
用户空间 → vmsplice() → pipe → splice() → 文件
         (页面引用)          (直接DMA)
共 0 次数据拷贝，只更新页表
```

**性能提升：**
- 减少 CPU 周期
- 减少内存带宽占用
- 对大内存进程特别有效（GB 级内存）

### 5.3 Soft-Dirty 页面跟踪

**增量 Checkpoint：**

```c
// 1. 清除 soft-dirty 位
int fd = open("/proc/PID/clear_refs", O_WRONLY);
write(fd, "4", 1);

// 2. 第一次完整 dump

// 3. 进程继续运行，访问的页面会设置 PME_SOFT_DIRTY

// 4. 第二次只 dump 脏页
for (addr = vma->start; addr < vma->end; addr += PAGE_SIZE) {
    u64 pme = pagemap[PAGE_PFN(addr)];
    if (pme & PME_SOFT_DIRTY) {
        // 这个页面被修改过，需要 dump
    }
}
```

### 5.4 TCP_REPAIR 机制

**TCP 连接状态保存和恢复：**

```c
// Dump 时
int on = 1;
setsockopt(fd, SOL_TCP, TCP_REPAIR, &on, sizeof(on));

// 获取序列号
struct tcp_repair_opt opts[4];
getsockopt(fd, SOL_TCP, TCP_REPAIR_OPTIONS, opts, &len);

// 获取队列数据
setsockopt(fd, SOL_TCP, TCP_REPAIR_QUEUE, TCP_SEND_QUEUE);
read(fd, send_queue_buf, len);

// Restore 时
socket(...);
bind(...);
connect(...);  // 建立连接
setsockopt(fd, SOL_TCP, TCP_REPAIR, &on);
setsockopt(fd, SOL_TCP, TCP_QUEUE_SEQ, &seq);  // 恢复序列号
write(fd, send_queue_buf, len);  // 恢复队列
setsockopt(fd, SOL_TCP, TCP_REPAIR, &off);
```

### 5.5 Namespace 处理

**7 种命名空间：**

1. PID NS - 进程 ID 隔离
2. Mount NS - 文件系统挂载隔离
3. Network NS - 网络栈隔离
4. IPC NS - System V IPC 隔离
5. UTS NS - 主机名隔离
6. User NS - UID/GID 隔离
7. Cgroup NS - Cgroup 视图隔离
8. Time NS - 时间隔离（Linux 5.6+）

**恢复策略：**

```c
// 方式 1: 创建新的命名空间
unshare(CLONE_NEWNS | CLONE_NEWNET | ...);

// 方式 2: 加入已存在的命名空间
int ns_fd = open("/proc/PID/ns/mnt", O_RDONLY);
setns(ns_fd, CLONE_NEWNS);
```

---

## 6. 用户空间 vs 内核空间对比

### 6.1 用户空间实现的优势

**CRIU 选择用户空间的原因：**

1. **灵活性** - 无需修改内核，快速迭代
2. **安全性** - 用户空间崩溃不影响内核
3. **可维护性** - 使用标准工具和库
4. **可移植性** - 跨架构支持容易

### 6.2 用户空间的局限性

**性能开销：**

```
用户空间 CRIU 的开销：
├─ ptrace 系统调用（每次暂停/继续）
├─ /proc 文件系统访问
├─ 用户态/内核态频繁切换
├─ Parasite 注入开销
└─ 数据拷贝（即使用 vmsplice）
```

**无法访问的状态：**
- task_struct 私有字段
- 某些内核对象的完整状态
- 内核线程

### 6.3 内核空间实现的优势

**假设在内核模块中实现：**

```
内核模块的优势：
├─ 无需 ptrace - 直接访问 task_struct
├─ 无需 /proc - 直接读取内核数据结构
├─ 无用户态/内核态切换
├─ 无需 Parasite 注入
├─ 真正的零拷贝 - 直接操作页表
└─ 原子操作 - 内核锁机制
```

**完整的状态访问：**

```c
// 内核模块可以直接访问：
struct task_struct *task;
├─ task->mm         // 完整的内存描述符
├─ task->files      // 文件表
├─ task->nsproxy    // 命名空间代理
├─ task->signal     // 信号结构
└─ ... 所有私有字段
```

### 6.4 内核空间实现的挑战

**开发复杂度：**

```
内核开发的挑战：
├─ 必须遵循内核编码规范
├─ 不能使用标准 C 库
├─ 不能使用浮点运算
├─ 必须处理所有错误
├─ 调试困难
├─ 内存管理复杂
└─ 并发控制复杂
```

**其他挑战：**
- 内核 API 不稳定（不同版本）
- 安全风险更大
- 需要自己实现序列化
- 用户空间交互复杂

---

## 总结

CRIU 是一个极其复杂的系统，涉及：

- **93,222 行**核心 C 代码
- **8 个架构**支持
- **100+ 种**资源类型
- **Linux 几乎所有子系统**

要在内核空间实现等价功能，需要：

1. **深入理解** Linux 内核内部机制
2. **掌握** 内核编程规范和 API
3. **实现** 复杂的同步和状态管理
4. **处理** 各种边界情况和错误

这是一个极具挑战性但很有学习价值的项目。

---

**下一步：**

基于这个深度分析，我们将设计内核模块版本的架构，采用渐进式实现策略：

- Phase 1: 单线程进程的基础 C/R
- Phase 2: 多线程支持和文件描述符
- Phase 3: 进程树、信号、定时器
- Phase 4: 高级特性（namespace、cgroup、网络）

详见后续设计文档。
