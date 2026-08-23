# Linux 内核模块实现 Checkpoint/Restore 功能 - 设计大纲

> 本文档是完整设计规范的大纲，用于确认设计方向和内容结构。
> 
> 项目目标：在 Linux 内核空间实现与 CRIU 用户空间版本功能对等的 checkpoint/restore 机制
> 实现策略：渐进式学习路径，分 4 个阶段实现

---

## 文档结构

### 第一部分：项目概述

#### 1.1 项目背景
- CRIU 用户空间实现的现状
- 为什么考虑内核空间实现
- 项目目标和范围

#### 1.2 设计原则
- 完整功能对等（与 CRIU 对标）
- 渐进式实现（4 个阶段）
- 可维护性和可扩展性
- 性能优先（充分利用内核空间优势）
- 安全性考虑

#### 1.3 技术选型
- 内核版本要求（建议 Linux 5.10+ LTS）
- 开发工具链
- 测试框架

---

### 第二部分：整体架构设计

#### 2.1 模块架构图
```
┌─────────────────────────────────────────────────┐
│           User Space Applications                │
└───────────────────┬─────────────────────────────┘
                    │ ioctl / sysfs / netlink
┌───────────────────▼─────────────────────────────┐
│         Kernel Module: criu_kernel.ko            │
│  ┌───────────────────────────────────────────┐  │
│  │    Control Interface (criu_ctl)           │  │
│  ├───────────────────────────────────────────┤  │
│  │    Checkpoint Engine (criu_dump)          │  │
│  │  ├─ Process Tree Walker                   │  │
│  │  ├─ Memory Snapshot                       │  │
│  │  ├─ FD Collector                          │  │
│  │  └─ Resource Serializer                   │  │
│  ├───────────────────────────────────────────┤  │
│  │    Restore Engine (criu_restore)          │  │
│  │  ├─ Process Tree Recreator                │  │
│  │  ├─ Memory Restorer                       │  │
│  │  ├─ FD Restorer                           │  │
│  │  └─ State Applier                         │  │
│  ├───────────────────────────────────────────┤  │
│  │    Serialization Layer                    │  │
│  │  ├─ Binary Format Handler                 │  │
│  │  └─ Image File Manager                    │  │
│  ├───────────────────────────────────────────┤  │
│  │    Helper Subsystems                      │  │
│  │  ├─ Task Freezer                          │  │
│  │  ├─ Namespace Manager                     │  │
│  │  └─ Resource Tracker                      │  │
│  └───────────────────────────────────────────┘  │
└───────────────────┬─────────────────────────────┘
                    │ 直接访问内核数据结构
┌───────────────────▼─────────────────────────────┐
│              Linux Kernel Core                   │
│  task_struct | mm_struct | files_struct | ...   │
└──────────────────────────────────────────────────┘
```

#### 2.2 核心组件说明
- Control Interface - 用户空间交互接口
- Checkpoint Engine - Dump 实现
- Restore Engine - Restore 实现
- Serialization Layer - 数据序列化
- Helper Subsystems - 辅助子系统

#### 2.3 与 CRIU 用户空间版本的对比
| 功能 | CRIU 用户空间 | 内核模块版本 |
|------|--------------|-------------|
| 进程控制 | ptrace | 直接访问 task_struct |
| 内存访问 | /proc + parasite | 直接操作 mm_struct |
| 数据传输 | vmsplice/splice | 零拷贝（页表操作）|
| 序列化 | Protobuf | 自定义二进制格式 |
| ... | ... | ... |

---

### 第三部分：核心数据结构设计

#### 3.1 进程树表示
```c
struct criu_pstree_node {
    pid_t pid;
    pid_t tgid;
    struct task_struct *task;  // 内核 task 指针
    
    struct criu_pstree_node *parent;
    struct list_head children;
    struct list_head siblings;
    
    int nr_threads;
    struct criu_thread_info *threads;
    
    // Checkpoint 状态
    struct criu_task_core_image *core_img;
    struct criu_mm_image *mm_img;
    struct criu_files_image *files_img;
    
    // 元数据
    unsigned long flags;
    struct criu_ns_ids ns_ids;
};
```

#### 3.2 内存快照结构
```c
struct criu_mm_image {
    // VMA 列表
    struct list_head vma_list;
    int nr_vmas;
    
    // 页面数据
    struct criu_pages_info {
        unsigned long nr_pages;
        struct page **pages;      // 页面指针数组
        unsigned long *pfns;      // 页帧号数组
    } page_data;
    
    // 内存描述符信息
    unsigned long start_code, end_code;
    unsigned long start_data, end_data;
    unsigned long start_brk, brk;
    unsigned long start_stack;
    unsigned long arg_start, arg_end;
    unsigned long env_start, env_end;
};
```

#### 3.3 镜像文件格式
```c
// 镜像文件头
struct criu_image_header {
    u32 magic;           // 魔数：'CRIU'
    u32 version;         // 版本号
    u32 type;            // 镜像类型
    u64 size;            // 数据大小
    u64 checksum;        // 校验和
};

// 镜像类型枚举
enum criu_image_type {
    CRIU_IMG_PSTREE,     // 进程树
    CRIU_IMG_CORE,       // 核心状态
    CRIU_IMG_MM,         // 内存映射
    CRIU_IMG_PAGES,      // 页面内容
    CRIU_IMG_FILES,      // 文件描述符
    // ...
};
```

#### 3.4 其他关键结构
- VMA 描述符
- 文件描述符表
- 信号队列
- 定时器
- Namespace IDs
- Cgroup 信息

---

### 第四部分：Checkpoint 实现设计

#### 4.1 Checkpoint 总体流程
```
criu_do_checkpoint(pid_t root_pid)
  ├─> criu_freeze_tasks()           // 冻结进程树
  ├─> criu_collect_pstree()         // 收集进程树
  ├─> criu_collect_resources()      // 收集资源
  │    ├─> collect_mm()             // 内存
  │    ├─> collect_files()          // 文件
  │    ├─> collect_signals()        // 信号
  │    └─> collect_namespaces()     // 命名空间
  ├─> criu_serialize_and_save()     // 序列化保存
  └─> criu_unfreeze_tasks()         // 解冻进程
```

#### 4.2 进程树遍历算法
- 使用内核的 `for_each_process()` 宏
- 线程遍历：`for_each_thread()`
- 构建进程树关系

#### 4.3 内存快照技术
**关键技术点：**
- 直接访问 `task->mm->mmap`（VMA 链表）
- 使用 `follow_page()` 获取物理页
- 零拷贝技术：直接引用页面，增加引用计数
- COW 处理：识别共享页面

**实现伪代码：**
```c
int criu_dump_memory(struct task_struct *task, struct criu_mm_image *img)
{
    struct mm_struct *mm = task->mm;
    struct vm_area_struct *vma;
    
    // 遍历 VMA
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
        // 保存 VMA 元数据
        save_vma_metadata(vma, img);
        
        // 遍历页面
        for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
            struct page *page = follow_page(vma, addr, FOLL_GET);
            if (page) {
                // 保存页面引用
                save_page_reference(page, img);
            }
        }
    }
    
    return 0;
}
```

#### 4.4 文件描述符收集
- 遍历 `task->files->fdt->fd`
- 处理不同类型的文件：
  - 常规文件
  - 管道
  - Socket
  - Eventfd
  - ...

#### 4.5 其他资源收集
- 寄存器状态（从 `task->thread`）
- 信号队列（`task->signal->shared_pending`）
- 定时器（`task->signal->posix_timers`）
- Namespace（`task->nsproxy`）

#### 4.6 数据序列化与保存
- 二进制格式定义
- 内核文件 I/O（`kernel_write()`）
- 大文件处理（分段写入）

---

### 第五部分：Restore 实现设计

#### 5.1 Restore 总体流程
```
criu_do_restore(const char *image_dir)
  ├─> criu_load_images()            // 加载镜像
  ├─> criu_prepare_namespaces()     // 准备命名空间
  ├─> criu_recreate_pstree()        // 重建进程树
  │    └─> kernel_thread() 创建进程
  ├─> criu_restore_resources()      // 恢复资源
  │    ├─> restore_mm()             // 内存
  │    ├─> restore_files()          // 文件
  │    ├─> restore_signals()        // 信号
  │    └─> restore_regs()           // 寄存器
  └─> criu_resume_tasks()           // 恢复执行
```

#### 5.2 进程创建策略
**挑战：** 内核模块无法直接 `fork()`

**解决方案：**
```c
// 使用 kernel_thread() + exec_usermodehelper()
struct task_struct *criu_create_task(pid_t target_pid)
{
    struct task_struct *task;
    
    // 方案 1: 使用 kernel_thread
    task = kthread_create(criu_task_fn, task_data, "criu_restored");
    
    // 方案 2: 通过用户空间 helper
    // exec_usermodehelper() + 参数传递
    
    // 设置 PID（需要 PID namespace 支持）
    // ...
    
    return task;
}
```

#### 5.3 内存恢复技术
```c
int criu_restore_memory(struct task_struct *task, struct criu_mm_image *img)
{
    struct mm_struct *mm;
    
    // 1. 创建新的 mm_struct
    mm = mm_alloc();
    
    // 2. 恢复 VMA
    for_each_vma_in_image(vma_img, img) {
        struct vm_area_struct *vma;
        
        // 创建 VMA
        vma = vm_area_alloc(mm);
        vma->vm_start = vma_img->start;
        vma->vm_end = vma_img->end;
        vma->vm_flags = vma_img->flags;
        
        // 插入 VMA
        insert_vm_struct(mm, vma);
    }
    
    // 3. 恢复页面内容
    for_each_page_in_image(page_img, img) {
        struct page *page = alloc_page(GFP_KERNEL);
        // 复制数据
        copy_from_image(page, page_img);
        // 映射到进程地址空间
        vm_insert_page(vma, addr, page);
    }
    
    // 4. 设置 mm
    task->mm = mm;
    
    return 0;
}
```

#### 5.4 文件描述符恢复
- 在内核中重新打开文件
- 使用 `filp_open()` 或 `sock_create()`
- 恢复文件位置（`lseek`）

#### 5.5 寄存器和执行恢复
```c
// 设置进程的寄存器状态
void criu_restore_regs(struct task_struct *task, struct pt_regs *saved_regs)
{
    struct pt_regs *regs = task_pt_regs(task);
    
    // 恢复寄存器
    *regs = *saved_regs;
    
    // 恢复 IP（指令指针）
    regs->ip = saved_regs->ip;
    
    // 唤醒任务
    wake_up_process(task);
}
```

---

### 第六部分：渐进式实现计划

#### Phase 1: 单线程进程基础 C/R（3-4 周）

**目标：** 实现最小可行产品（MVP）

**范围：**
- ✅ 单线程进程
- ✅ 基本内存（堆、栈、代码段）
- ✅ 基本寄存器状态
- ✅ 简单的文件描述符（stdin/stdout/stderr）
- ❌ 不支持：多线程、网络、复杂文件系统、namespace

**里程碑：**
1. 周 1-2：模块框架 + 控制接口
2. 周 2-3：Checkpoint 基础实现
3. 周 3-4：Restore 基础实现
4. 周 4：测试和调试

**可演示成果：**
```bash
# 测试程序：简单的计数器
./test_program &
PID=$!

# Checkpoint
echo $PID > /sys/kernel/criu/checkpoint

# Kill 进程
kill $PID

# Restore
echo /tmp/criu_$PID > /sys/kernel/criu/restore

# 进程继续从 checkpoint 点运行
```

#### Phase 2: 多线程和文件支持（4-5 周）

**新增功能：**
- ✅ 多线程进程
- ✅ 线程本地存储（TLS）
- ✅ 互斥锁、条件变量状态
- ✅ 更多文件类型：
  - 常规文件（完整位置和标志）
  - 管道（包括数据）
  - Unix domain socket

**技术难点：**
- 线程同步状态的保存和恢复
- Futex 状态
- 管道中的数据

#### Phase 3: 进程树和高级资源（5-6 周）

**新增功能：**
- ✅ 完整进程树（父子进程）
- ✅ 信号队列和处理器
- ✅ POSIX 定时器
- ✅ Interval 定时器
- ✅ 资源限制（rlimit）
- ✅ Session 和进程组

**技术难点：**
- PID 恢复（在内核中实现）
- 进程树的拓扑关系
- 信号的准确恢复

#### Phase 4: 高级特性（6-8 周）

**新增功能：**
- ✅ Namespace 支持（PID/Mount/Network/...）
- ✅ Cgroup 集成
- ✅ TCP/UDP Socket 状态保存
- ✅ 共享内存（SysV IPC、POSIX）
- ✅ 内存映射文件
- ✅ SELinux/AppArmor 上下文

**技术难点：**
- Namespace 的创建和管理（在内核中）
- TCP 连接状态（类似 TCP_REPAIR）
- 复杂的内存共享关系

---

### 第七部分：关键技术难点与解决方案

#### 7.1 在内核中创建用户进程
**问题：** 内核模块无法直接 `fork()`

**解决方案：**
1. 使用 `kernel_thread()` 创建内核线程
2. 通过 `exec_usermodehelper()` 执行用户程序
3. 或者：创建最小的用户空间 helper 程序协助

#### 7.2 PID 恢复
**问题：** 如何在内核中恢复特定 PID

**解决方案：**
1. 使用 PID namespace 隔离
2. 操作 `struct pid` 和 PID 分配器
3. 需要深入理解 PID 管理机制

#### 7.3 数据序列化格式
**问题：** 内核中无 Protobuf

**解决方案：**
1. 设计简单的二进制格式
2. 使用 TLV（Type-Length-Value）编码
3. 或者：使用 JSON（性能较差但易调试）

#### 7.4 用户空间交互
**问题：** 如何传递大量数据（如镜像文件路径、参数）

**解决方案：**
1. **ioctl**：适合小量参数
2. **sysfs**：适合配置和状态查询
3. **netlink**：适合复杂的双向通信
4. **debugfs**：开发阶段使用

**推荐方案：** ioctl + sysfs 组合

#### 7.5 内核内存限制
**问题：** 内核内存有限，大进程如何处理

**解决方案：**
1. 分段处理内存
2. 使用 vmalloc 分配虚拟连续内存
3. 实现增量 checkpoint

#### 7.6 安全性问题
**问题：** 内核模块权限过大

**解决方案：**
1. 严格的权限检查（CAP_SYS_ADMIN）
2. 输入验证
3. 防止 TOCTOU 攻击
4. 使用 SELinux/AppArmor 策略

---

### 第八部分：测试策略

#### 8.1 单元测试
- 每个子模块的独立测试
- Mock 内核接口
- 使用内核测试框架（KUnit）

#### 8.2 集成测试
- 端到端测试用例
- 参考 CRIU 的 ZDTM 测试套件
- 测试程序示例：
  - 计数器程序
  - 文件读写程序
  - 多线程程序
  - Socket 通信程序

#### 8.3 压力测试
- 大内存进程
- 大量文件描述符
- 深进程树

#### 8.4 回归测试
- 每次提交运行完整测试套件
- CI/CD 集成

---

### 第九部分：性能优化

#### 9.1 性能目标
- Checkpoint 时间：< 100ms（小进程）
- Restore 时间：< 200ms（小进程）
- 内存开销：< 原进程内存的 10%

#### 9.2 优化点
- 零拷贝技术（页表操作）
- 并行处理（多核利用）
- 增量 checkpoint
- 压缩（可选）

---

### 第十部分：文档和示例

#### 10.1 开发文档
- 模块编译和安装指南
- API 参考手册
- 内核接口说明

#### 10.2 用户文档
- 使用教程
- 示例程序
- 故障排除

#### 10.3 代码示例
- Checkpoint 示例
- Restore 示例
- 用户空间工具示例

---

### 第十一部分：项目管理

#### 11.1 开发环境
- 推荐：Ubuntu 22.04 LTS + Linux 5.15 内核
- 开发工具：GCC, Make, Git
- 调试工具：QEMU + GDB, ftrace, crash

#### 11.2 代码仓库结构
```
criu_module/
├── criu/                    # CRIU 原始代码（参考）
├── kernel_module/           # 内核模块源码
│   ├── core/               # 核心功能
│   ├── checkpoint/         # Checkpoint 实现
│   ├── restore/            # Restore 实现
│   ├── serialize/          # 序列化
│   └── include/            # 头文件
├── userspace/              # 用户空间工具
├── tests/                  # 测试程序
├── docs/                   # 文档
└── scripts/                # 构建和测试脚本
```

#### 11.3 开发流程
1. 功能设计 → 代码审查
2. 实现 → 单元测试
3. 集成 → 集成测试
4. 文档更新

---

## 附录

### A. 参考资料
- Linux 内核源码（mm/, kernel/, fs/）
- CRIU 项目文档
- LWN.net 相关文章
- 内核模块开发指南

### B. 术语表
- C/R: Checkpoint/Restore
- PIE: Position Independent Executable
- VMA: Virtual Memory Area
- ...

### C. 常见问题
- Q: 为什么不直接使用 CRIU？
- Q: 内核模块是否会进入主线？
- Q: 性能提升有多少？
- ...

---

**下一步：** 基于此大纲编写完整的详细设计文档

