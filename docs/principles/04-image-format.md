# 原理 04 —— CRIU 的镜像格式

> 被引用于:[A3](../steps/A3-minimal-dump.md)、[B1](../steps/B1-mini-restore.md)

---

## 1. 为什么要照抄别人的格式

本项目有一条硬约束:**镜像必须与 CRIU 二进制兼容。**

理由不是 protobuf 有多好,而是一个纯粹工程上的杠杆:

```
兼容 CRIU 格式  ⇒  可以用真 criu restore 当验证器
               ⇒  可以用 crit decode 看自己产出的东西对不对
               ⇒  可以用 489 个 ZDTM 测试程序
               ⇒  dump 和 restore 两侧可以独立开发
```

如果自创格式,那么在写出 restore 之前,**dump 的正确性完全无法验证**。你会有
一条长长的串联依赖:写完 dump 的九个部分,再写完 restore 的九个部分,然后第一次
运行,然后面对一个「不知道错在哪」的失败。

**格式兼容性买到的不是优雅,是可测试性。** 这是本项目最重要的一个工程决定。

---

## 2. 文件布局

一次 dump 产出一个目录,里面是若干 `.img` 文件。文件名有三种形式:

| 形式 | 含义 | 例子 |
|---|---|---|
| `<name>.img` | 全局的,一次 dump 一份 | `inventory.img`、`pstree.img`、`files.img` |
| `<name>-<pid>.img` | 每任务一份 | `core-1234.img`、`mm-1234.img` |
| `<name>-<id>.img` | 每对象一份,id 是内部分配的 | `fdinfo-2.img` |

一个极简单线程进程的完整镜像集大约 12 个文件:

| 文件 | 内容 |
|---|---|
| `inventory.img` | 全局元信息:镜像版本、`root_ids` |
| `pstree.img` | 进程树平表(pid/ppid/pgid/sid/threads) |
| `core-$pid.img` | 寄存器、信号掩码、rlimit、TLS |
| `mm-$pid.img` | VMA 列表 + `brk`/`start_code`/`arg_start` 等 |
| `pages-1.img` | 页内容(裸字节流) |
| `pagemap-$pid.img` | 页内容的索引 |
| `files.img` | 全局文件表 |
| `fdinfo-$id.img` | fd → 文件表条目的映射 |
| `reg-files.img` | 常规文件的路径/flags/pos |
| `ids-$pid.img` | 各共享对象和 namespace 的 id 引用 |
| `fs-$pid.img` | cwd 和 root |
| `creds-$pid.img` | uid/gid/capabilities |

**第一天该做的事:对着目标进程跑一遍真 criu,把它产出的东西全解码出来当规格书。**

```bash
criu dump -t $PID -D /tmp/ref-imgs -v4 --leave-running
for f in /tmp/ref-imgs/*.img; do
	echo "=== $f ==="
	crit decode -i "$f" --pretty | head -40
done > /tmp/ref-decoded.txt
```

**`/tmp/ref-decoded.txt` 比任何文档都可靠**,因为它是那个即将验证你的程序的
实际输出。

---

## 3. 文件内部结构

### 通用格式

```
[4 bytes] magic          小端
然后重复零次或多次:
  [4 bytes] 后面这条 protobuf 消息的长度
  [N bytes] 序列化的 protobuf 消息
```

参照 `criu/criu/include/image.h` 里的 `IMG_COMMON_MAGIC` 和各类型的 magic 常量,
以及 `criu/criu/image.c` 的 `do_open_image()`。

**长度前缀是必需的**,因为 protobuf 的线格式本身不自带长度 —— 解析器无法知道
一条消息在哪结束。这是 protobuf 用于流式存储时的标准做法。

### 唯一的例外:`pages-N.img`

```
[4 bytes] magic
[所有剩下的] 裸页数据,一页接一页,没有任何元数据
```

理由是性能:这样 CRIU 可以用 `splice()` 零拷贝地把页内容写进去。掺任何元数据
都会破坏这一点。索引信息全部放在 `pagemap-$pid.img` 里。

### 漏掉 magic 会怎样

`criu restore` 报:

```
Unknown magic 0x0 on ...
```

这个错误信息很清楚,是 A3 第一天几乎必然会撞一次的错。撞了不用慌。

---

## 4. protobuf:为什么是它,以及它的两个陷阱

### 为什么

`criu/images/` 下有 76 个 `.proto` 文件,它们**就是格式的规格说明**。没有别的
文档,proto 文件本身是权威。

protobuf 的两个性质对 C/R 特别合适:

1. **未知字段被跳过。** 一个老版本的 criu 读到新版本加的字段会忽略它,其余照常
   解析。这让格式可以向前演进。
2. **optional 字段可以缺席。** 不支持某个特性时不写对应字段,而不是写一个假值。

第 1 点有一个直接的实践后果:**X1 启用被注释掉的字段 19 不会破坏兼容性** —— 
真 criu 会跳过它。

### 陷阱一:字段号是契约,字段名不是

```protobuf
	required uint32			pid		= 1;
```

线格式里只有 `1`,没有 `pid` 这个字符串。所以:

- 改字段名:兼容
- 改字段号:**不兼容**,而且症状是「值被解析到错误的字段上」

### 陷阱二:`required` 缺席 = 解析失败

proto2 的 `required` 字段如果没写,解析会**直接失败**而不是给默认值。这实际上
是好事 —— 它让「忘记填某个字段」变成一个响亮的错误而不是静默的零值。

`criu restore` 因此会在你漏填必需字段时给出明确报错,而不是恢复出一个错误的进程。

---

## 5. 在内核里做 protobuf 编码

这是本项目在格式上唯一真正的技术难点。

### 方案:用 protobuf-c 生成代码

```bash
protoc-c --c_out=kernel_module/images -Icriu/images criu/images/core.proto
```

生成的 `*.pb-c.c` 只依赖 `malloc`/`free`/`memcpy`/`strlen`/`assert`。
protobuf-c 支持自定义 allocator:

```c
/* protobuf-c lets us supply an allocator; point it at the slab so we never
 * call libc malloc (which does not exist here). */
static void *criu_pb_alloc(void *data, size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void criu_pb_free(void *data, void *ptr)
{
	kfree(ptr);
}

ProtobufCAllocator criu_pb_allocator = {
	.alloc = criu_pb_alloc,
	.free = criu_pb_free,
	.allocator_data = NULL,
};
```

**不要手写 protobuf 编码。** varint 编码、zigzag、字段顺序、packed repeated 的
处理,足够耗掉一周,而且每个 bug 的症状都是「`criu restore` 报一个看不懂的错」。

### 必须核实的一件事:生成的代码里没有浮点

内核态使用 SSE/浮点寄存器需要显式 `kernel_fpu_begin()`/`kernel_fpu_end()`,
否则会破坏用户态的 FPU 状态。protobuf 的 `double`/`float` 字段会生成浮点代码。

我们用到的 proto 里应该没有浮点字段,但**这必须核实,不能假设**:

```bash
# No floating point in kernel-side generated code: using SSE without
# kernel_fpu_begin() corrupts userspace FPU state.
objdump -d kernel_module/images/*.o | grep -E '(mov|add|mul)s[sd]|xmm' && \
	{ echo "FAIL: floating point in generated code"; exit 1; }
echo "no floating point: OK"
```

---

## 6. 两级间接:id 是怎么表达共享的

镜像格式里最需要想明白的一个设计,是它怎么表示「两个东西是同一个东西」。

以 fd 为例,是**两层**的:

```
fdinfo-$id.img:  fd 3 → file id 0x1234
                 fd 4 → file id 0x1234   ← 同一个 id ⇒ 这是一个 dup
files.img:       id 0x1234 → type=REG,指向 reg-files 里的条目
reg-files.img:   path=/tmp/x, flags=O_RDWR, pos=42
```

**中间那层 file id 就是去重机制。** 内核里两个 fd 指向同一个 `struct file`
⇒ 分配同一个 id ⇒ restore 端知道要 `dup()` 而不是 `open()` 两次。

同样的模式用在所有共享资源上。`criu/images/ids.proto`:

```protobuf
message task_kobj_ids_entry {
	required uint32			vm_id		= 1;
	required uint32			files_id	= 2;
	required uint32			fs_id		= 3;
	required uint32			sighand_id	= 4;
	...
}
```

**这五个 id 就是五种 `CLONE_*` 共享的镜像表示。** 两个 task 的 `files_id` 相等
⇔ 它们 `CLONE_FILES` 共享 ⇔ restore 时第二个用 `CLONE_FILES` clone 出来。

在内核侧,分配这些 id 就是**给指针建一张映射表**:

```c
	/* Pointer identity is the dedup key. CRIU has to ask the kernel one pair
	 * at a time with kcmp(); we just compare pointers. */
	id = criu_objmap_get(map, task->files, &is_new);
```

`is_new` 出参把「去重」和「每个对象只写一次」统一成了一个操作:

```c
	id = criu_objmap_get(map, file, &is_new);
	write_fdinfo(fd, id);		/* always reference the id */
	if (is_new)
		write_file_entry(id, file);	/* but describe it once */
```

---

## 7. `crit`:必须会用的工具

`crit` 是 CRIU 自带的镜像检查工具(`criu/crit/`)。

| 命令 | 用途 | 位置 |
|---|---|---|
| `crit decode -i f.img --pretty` | 镜像 → JSON | `criu/crit/crit/__main__.py:1608` |
| `crit encode` | JSON → 镜像 | 同上 `:1625` |
| `crit info f.img` | 摘要 | 同上 `:1638` |
| `crit x <dir> <what>` | 探索整个镜像目录 | 同上 `:1643` |

`crit x <dir> ps` 会以进程树的形式展示,比逐个 decode 直观。

**开发期最有用的一条命令**是把自己的产出和真 criu 的产出对比:

```sh
crit decode -i /tmp/ref/core-$PID.img  --pretty > /tmp/ref.json
crit decode -i /tmp/ours/core-$PID.img --pretty > /tmp/ours.json
diff -u /tmp/ref.json /tmp/ours.json
```

但要注意:**有些字段合法地不同**(`dump_uptime`、`dump_criu_run_id` 是每次运行
都变的)。所以这个对比是**观察工具,不该做成门禁** —— 做成门禁会让你去追一堆
无害的差异(比如 CRIU 写了某个 optional 字段而你没写,但 restore 并不需要它)。

**真正的门禁只有一个:`criu restore` 能不能成功。**

---

## 8. 镜像版本与 inventory

`inventory.img` 是第一个被读的文件,它携带 `img_version`。版本不匹配时
`criu restore` 直接拒绝。

`inventory_entry` 的字段(`criu/images/inventory.proto`)包括 `img_version`、
`root_ids`、`dump_uptime`、`dump_criu_run_id`、`compress` 等。

**必须写对 `img_version`**,而且要写你实际对齐的那个 criu 版本的值。写错的症状
是 restore 在第一步就拒绝,报版本不匹配 —— 这个错误很友好,一眼能看懂。

---

## 9. 调试:镜像不被接受时怎么二分

`criu restore` 失败时最有效的手段:

```sh
# Binary-search which image is wrong by swapping in the reference version
# one at a time. 12 images means about 4 experiments.
cp /tmp/ref/core-$PID.img /tmp/ours/    # replace core, see if the error moves
criu restore -D /tmp/ours -v4
```

**这把「12 个镜像里哪个错了」从猜变成了 log₂(12) ≈ 4 次实验。** 因为镜像是磁盘上
独立的文件,可以自由混搭 —— 这是文件式接口带来的又一个好处。

---

## 10. 延伸阅读

- `criu/images/*.proto` —— 76 个文件,**它们就是规格书**
- `criu/criu/include/image.h` —— magic 常量
- `criu/criu/image.c` —— 镜像打开/读写的实现
- `criu/crit/crit/__main__.py` —— crit 的实现,想知道某个字段怎么解读时可以读
- https://protobuf.dev/programming-guides/encoding/ —— 线格式,想理解为什么需要
  长度前缀时值得读
- [03-memory-and-vma](03-memory-and-vma.md) —— pages/pagemap 的分工细节
- [07-fd-and-shared-objects](07-fd-and-shared-objects.md) —— 两级间接的完整故事
