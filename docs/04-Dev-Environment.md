# 开发环境与 CI

## 0. 一个前提:宿主是 macOS

内核模块开发必须在 Linux 上做,而且**必须能随时崩溃重启**。所以本机 macOS 只当
编辑器和 git 宿主,所有编译/加载/测试都在虚拟机里。

推荐 **Lima**(轻量、CLI 友好、和 Docker 生态通):

```bash
brew install lima
limactl start --name=criu-dev --cpus 4 --memory 8 --disk 60 template://ubuntu-22.04
limactl shell criu-dev
```

Apple Silicon 上注意:Lima 默认起 **aarch64** 客户机。本项目是 **x86_64 only**
(`rt_sigframe` 是 arch-specific),所以要么用 x86_64 客户机(Rosetta/TCG 模拟,慢
但可用),要么直接用一台 x86_64 云主机。**推荐后者** —— 内核编译在模拟环境下会慢
到影响心情。

```bash
# x86_64 客户机(慢,仅在没有云主机时用)
limactl start --name=criu-dev --arch x86_64 --cpus 4 --memory 8 template://ubuntu-22.04
```

## 1. 三层测试环境

| 层 | 环境 | 崩溃代价 | 用在哪 |
|---|---|---|---|
| L1 编译期 | 任意 Linux,只需内核头文件 | 无 | 每次保存。`make` + sparse + checkpatch |
| L2 虚拟机 | virtme-ng 或 QEMU + 自建 5.10.29 | 重启 10 秒 | 每次 `insmod`。**默认工作环境** |
| L3 裸机 | 一台可牺牲的 x86_64 机器 | 重装 | 只在性能测量时用 |

**永远不要在 L3 或你的开发机上直接 insmod。** 一个错误的 `vma->vm_next` 解引用
就是不可恢复的 oops,运气不好会损坏文件系统。

### L2 首选:virtme-ng

virtme-ng 直接用**宿主的根文件系统**启动你编译的内核,不用做 rootfs 镜像,启动
约 2 秒。这是内核模块开发迭代最快的方式。

```bash
sudo apt install -y virtme-ng
# 编译内核(第一次约 20-40 分钟)
./scripts/build-kernel.sh 5.10.29
# 启动:自动挂载当前目录,内核是你刚编的
vng --run ~/kernels/linux-5.10.29 --rwdir=$(pwd)
```

进去之后:

```bash
insmod kernel_module/criu_kernel.ko
dmesg | tail -20
```

### L2 备选:QEMU + initramfs

virtme-ng 装不上时用 `scripts/run-qemu.sh`。它需要一个 initramfs;
`scripts/build-kernel.sh` 会顺手用 busybox 生成一个。

## 2. 内核源码树

```bash
mkdir -p ~/kernels && cd ~/kernels
curl -LO https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.10.29.tar.xz
tar xf linux-5.10.29.tar.xz
```

模块的 `Makefile` 通过 `KDIR` 指向它:

```make
KDIR ?= $(HOME)/kernels/linux-5.10.29
```

## 3. 必需的内核配置

`scripts/build-kernel.sh` 会设置这些。逐条说明为什么需要:

| 配置项 | 为什么 |
|---|---|
| `CONFIG_MODULES=y` | 废话,但也得写 |
| `CONFIG_KALLSYMS=y` | oops 栈回溯里有函数名,否则只有裸地址 |
| `CONFIG_DEBUG_INFO=y` | gdb/crash 能看变量 |
| `CONFIG_DEBUG_INFO_DWARF4=y` | 老 gdb 对 DWARF5 支持差 |
| `CONFIG_GDB_SCRIPTS=y` | 提供 `lx-*` gdb 命令,能直接 dump task 列表 |
| `CONFIG_FRAME_POINTER=y` | 栈回溯准确 |
| `CONFIG_DEBUG_KERNEL=y` | 开启下面各项的前置 |
| `CONFIG_DEBUG_VM=y` | **关键**。VMA/页操作的内部一致性断言。你写的是 mm 代码,这个必开 |
| `CONFIG_DEBUG_VM_RB=y` | 校验 VMA 红黑树一致性 |
| `CONFIG_PROVE_LOCKING=y` | lockdep。你会大量 `mmap_read_lock`,死锁会被提前抓到 |
| `CONFIG_DEBUG_ATOMIC_SLEEP=y` | 抓「持锁/原子上下文里睡眠」,新手最常犯 |
| `CONFIG_KASAN=y` | 越界与 use-after-free。慢 3 倍,但值得 |
| `CONFIG_DEBUG_LIST=y` | 链表操作校验 —— 你会大量遍历 VMA 链表 |
| `CONFIG_DEBUG_FS=y` | A1 的探针接口 |
| `CONFIG_CHECKPOINT_RESTORE=y` | CRIU 需要的 `/proc/*/map_files` 等 |
| `CONFIG_MEM_SOFT_DIRTY=y` | 增量 dump 需要 |
| `CONFIG_EXPERT=y` | 上面几项的可见性前置 |

**`CONFIG_DEBUG_VM` + `CONFIG_PROVE_LOCKING` + `CONFIG_DEBUG_ATOMIC_SLEEP` 这三个
是本项目的核心安全网。** 它们会把「碰巧没崩」的错误变成明确的 warning。不开这三个
写 mm 代码,等于闭眼开车。

## 4. 编译 CRIU 本体(必需 —— 它是你的验证器)

```bash
sudo apt install -y build-essential libprotobuf-dev libprotobuf-c-dev \
    protobuf-c-compiler protobuf-compiler python3-protobuf pkg-config \
    libcap-dev libnl-3-dev libnet-dev libaio-dev libbsd-dev \
    libgnutls28-dev libnftables-dev libdrm-dev python3-pip
cd criu && make -j$(nproc) && sudo make install
criu check --all   # 允许有 fail,记下来
```

`criu check --all` 的输出**要存档**。它列出当前内核支持哪些 C/R 特性,是你判断
「测试失败是我的 bug 还是内核不支持」的基线。

## 5. GitHub Actions CI

三个 workflow,按运行代价从小到大排。

### 5.1 `lint.yml` —— 每次 push,约 1 分钟

- `checkpatch.pl --no-tree --strict` 检查代码风格
- `sparse` 检查 `__user`/`__kernel` 指针混用(内核模块最危险的一类 bug)
- 检查提交信息里有 `Assisted-by:` 且**没有** `Signed-off-by:`

### 5.2 `build.yml` —— 每次 push,约 10 分钟

矩阵编译。**5.10 必须过,6.1 允许失败** —— 后者是刻意保留的哨兵,它一旦意外通过,
说明有人写了不该有的兼容层。

| 内核 | 期望 |
|---|---|
| 5.10.x | 必须过 |
| 5.15.x | 必须过 |
| 6.1.x | **允许失败**(maple tree,`vm_next` 不存在) |

用 `linux-headers-*` 包而不是完整源码树,几秒装完。

### 5.3 `qemu-test.yml` —— PR 与 main,约 20 分钟(缓存命中后 5 分钟)

**关键限制:GitHub Actions 标准 runner 没有嵌套虚拟化,`/dev/kvm` 不可用。**
所以 QEMU 只能跑 TCG 纯模拟模式,慢约 10 倍。应对:

1. 用 `actions/cache` 缓存编译好的 5.10.29 `bzImage`,key 里带配置文件哈希。
   只有改了内核配置才重编。
2. 测试程序极小(A3 的极简目标就是静态链接单线程),TCG 下也是秒级。
3. 完整 ZDTM 跑不动 —— CI 只跑一个**白名单子集**(`ci/zdtm-allowlist.txt`),
   完整套件在本地 L2 跑。

白名单机制同时是**进度仪表盘**:每完成一个步骤就往里加测试名。文件行数即进度。

## 6. 仓库布局

```
criu_module/
├── criu/                       # CRIU 上游源码(验证器 + 参照实现)
├── docs/
│   ├── 01-CRIU-Architecture-Analysis.md
│   ├── 02-Kernel-Module-Design-Outline.md
│   ├── 03-Iteration-Plan.md    # 大纲
│   ├── 04-Dev-Environment.md   # 本文件
│   ├── steps/                  # 每步骤一个文件
│   └── principles/             # 技术原理(学习资料)
├── kernel_module/
│   ├── Makefile
│   ├── core/                   # 模块框架、控制接口
│   ├── checkpoint/             # dump 实现
│   ├── serialize/              # protobuf-c 序列化
│   └── include/
├── userspace/
│   ├── criu-shim/              # 冒充 criu 二进制,给 ZDTM 用
│   └── mini-restore/           # B 轨
├── tests/
│   ├── progs/                  # 测试目标程序
│   └── compare/                # 对照 diff 脚本
├── scripts/
│   ├── build-kernel.sh
│   └── run-qemu.sh
├── ci/
│   └── zdtm-allowlist.txt      # CI 跑的 ZDTM 子集 = 进度仪表盘
└── .github/workflows/
    ├── lint.yml
    ├── build.yml
    └── qemu-test.yml
```

## 7. 调试手段速查

| 症状 | 用什么 |
|---|---|
| 模块加载即崩 | `dmesg`;virtme-ng 里崩了直接 Ctrl-C 重启 |
| 想单步内核代码 | `scripts/run-qemu.sh --gdb`,另一终端 `gdb vmlinux` → `target remote :1234` |
| 想看某进程的 VMA | gdb 里 `lx-ps`,然后 `p *((struct task_struct *)0xffff...)->mm` |
| 怀疑锁顺序错 | `dmesg | grep -A40 "possible circular locking"` |
| 想看函数调用路径 | `trace-cmd record -p function_graph -g criu_dump_memory` |
| 镜像格式不对 | `crit decode -i pages-1.img --pretty` |
| 镜像目录总览 | `crit x /tmp/imgs/ ps` 和 `crit info /tmp/imgs/core-1.img` |
