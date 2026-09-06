# 10 —— VMA 语义、页面内容与属性

## 1. VMA 描述什么

进程地址空间可以从两个层次观察：

```text
VMA：一段连续虚拟地址的映射语义
页表/struct page：这段地址当前实际对应的页面内容
```

VMA 不是页面数组，也不保证其中每一页都已经 fault。一个 VMA 内部可能同时存在：

- 尚未分配的页；
- 全零页；
- 已经 COW 的匿名页；
- 仍然来自文件的页；
- 不能通过普通 `struct page` 访问的特殊页。

因此，“VMA 类型”和“物理页当前是否被共享”是两个正交问题。

## 2. 四类普通 VMA

CRIU 对普通映射使用“是否有文件 backing”与“私有/共享”两个维度进行分类。

### 2.1 私有文件映射

典型来源是：

```c
mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
```

ELF 程序的 text、rodata、data、bss，以及动态链接器和共享库，也通常落在这类映射中。
`MAP_PRIVATE` 表示初始内容来自文件，写入时发生 COW。发生 COW 后，某一页的实际内容
可能已经是匿名页，但 VMA 的映射语义仍然是私有文件映射。

dump 时不能只根据 `vm_file` 决定是否保存页面：尚未 COW 的页可以从文件重建，已经
COW 的页必须保存当前内容。

### 2.2 共享文件映射

典型来源是：

```c
mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

共享关系由“文件 inode + 文件偏移 + 映射属性”定义。多个进程看到的是同一个文件对象，
所以 dump/restore 要保存文件身份、偏移、长度、权限和共享语义，而不是为每个进程重复
复制同一份文件 backing。

### 2.3 共享匿名映射

典型来源是：

```c
mmap(NULL, size, PROT_READ | PROT_WRITE,
     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
```

也可能由 `memfd_create()`、tmpfs 或 `/dev/shm` 等机制产生。它没有普通磁盘文件作为
用户可见 backing，但多个进程仍然共享同一个内存对象。内核实现可能使用 shmem，因此
不能用 `vma->vm_file == NULL` 单独判断匿名共享；应结合 `vma_is_anonymous()`、shmem
语义或等价的 5.10.29 版本判断。

共享匿名对象的页面内容只应保存一次，各进程的 VMA 记录引用同一个共享对象。这属于
跨进程资源收集和去重，不能由单进程 VMA 探针独立完成。

### 2.4 私有匿名映射

典型来源是：

```c
mmap(NULL, size, PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

`malloc()`、`brk()` 形成的 heap，以及主线程/工作线程的栈，通常也属于私有匿名映射。
`fork()` 后父子进程的匿名私有页可能暂时共享同一个物理页，但写入时会 COW；这不改变
VMA 的私有匿名语义。

## 3. 其他常见 VMA 区域

四类普通 VMA 之外，还要识别特殊映射或特殊属性：

| 区域或属性 | 典型含义 | 普通页 dump 策略 |
|---|---|---|
| vDSO / vvar | 内核提供的特殊用户映射 | 标记特殊类型，按 CRIU 约定处理 |
| `PROT_NONE` guard | 不可访问的保护区 | 保存 VMA 元数据，不读取页面内容 |
| `VM_GROWSDOWN` | 常见于栈的向下增长语义 | 保留映射语义 |
| `VM_DONTDUMP` | 默认不应 checkpoint 的区域 | 标记并按 CRIU 规则跳过 |
| `VM_HUGETLB` | hugetlb 页 | 需要独立页大小和资源处理 |
| `VM_IO` / `VM_PFNMAP` | 设备或 PFN 映射 | 不能套用普通 `struct page` 读取流程 |
| `VM_MIXEDMAP` | page 与 PFN 混合映射 | 必须识别，不能当普通匿名 VMA |
| `VM_LOCKED` | 页面被 mlock | 页面仍可按普通内容 dump，锁定状态另行恢复 |

透明大页不一定产生特殊 VMA；一个普通 VMA 内部可以同时包含普通页和 THP。因此页大小
和页面存在性仍应在页面遍历阶段判断。

## 4. 不要原样持久化 `vm_flags`

`vm_flags` 是 Linux 5.10.29 的内核内部 bit 集合，不应直接作为跨版本镜像 ABI。dump
应读取原始 flags，再转成稳定的语义字段，例如：

```text
prot          = READ | WRITE | EXEC
map_flags     = SHARED | PRIVATE | ANONYMOUS | GROWSDOWN
special       = NONE | VDSO | VVAR | HUGETLB | IO | PFNMAP
dump_policy   = NORMAL | DONTDUMP | PROT_NONE
locked        = 0 | 1
```

`VM_MAYREAD`、`VM_MAYWRITE`、`VM_MAYEXEC` 表示最大允许权限，不等于当前权限；
`VM_ACCOUNT` 等主要用于内核记账的字段通常不应成为恢复所需的核心字段。原始
`vm_flags` 可以保留在诊断输出中，方便和内核源码对照，但不应取代规范化字段。

## 5. 物理共享与 VMA 共享的区别

以下两种关系必须分开记录：

```text
VMA 语义共享：MAP_SHARED，决定进程间可见性和恢复映射方式
物理页暂时共享：fork/COW 或去重造成的 page 引用共享
```

例如，`fork()` 后的私有匿名 VMA 可能暂时引用同一物理页，但 restore 仍必须恢复为
两个私有映射；共享匿名 VMA 则必须恢复为同一个共享对象。A8 负责跨进程共享对象的
身份和去重，不能仅凭 VMA 类别推断所有物理页关系。

## 6. 文件移动与恢复依赖

文件映射的恢复依赖至少包括：

- 原始路径或可替代的文件定位方式；
- device/inode 等身份信息；
- 文件大小和必要的内容校验；
- 映射起始偏移、长度、权限及共享属性。

仅改名但 inode 未变，理论上可以通过重新定位恢复；如果镜像只保存原始路径，路径变化
仍会导致打开失败。文件被删除、内容改变、大小不匹配或 inode 不一致，都可能使恢复
失败。已经 COW 的私有文件页可以从 pages 镜像恢复，但共享文件映射仍依赖原文件对象
来维持共享和持久化语义。
