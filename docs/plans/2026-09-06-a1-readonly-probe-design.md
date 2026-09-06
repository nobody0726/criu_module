# A1 Read-only Probe Design

**Date:** 2026-09-06

**Status:** Approved

## Goal

Implement the first reusable read layer for the kernel-module dump track on Linux
5.10.29/aarch64. Given one target PID, the module must safely resolve and pin the
task, acquire its `mm_struct`, walk the 5.10 VMA linked list under the mmap read
lock, normalize VMA semantics, and expose a debugfs view that can be compared with
`/proc/PID/maps`.

A1 is a read-only probe. It does not freeze tasks, write CRIU images, restore a
process, walk children, or deduplicate resources shared by multiple processes.

## Constraints

- Target kernel is Linux 5.10.29 on aarch64; `mm->mmap`/`vm_next` are intentional.
- The module must not use `kallsyms_lookup_name()` or other dynamic symbol lookup.
- Task lookup uses `find_vpid()` + `pid_task()` under RCU and pins the task before
  leaving the RCU read-side critical section.
- Address-space access uses `get_task_mm()`, `mmap_read_lock()` and `mmput()`.
- Every debugfs entry checks `CAP_SYS_ADMIN` explicitly.
- A1 output is diagnostic/inspection data, not a CRIU image ABI.

## VMA classification

`vma_is_shmem()` is declared in the 5.10 headers but is implemented in
`mm/shmem.c` and is not an exported module symbol. The classifier therefore uses
the following order:

1. `VM_IO`, `VM_PFNMAP`, `VM_HUGETLB` and `VM_MIXEDMAP` are identified as special
   before ordinary classification.
2. `vma_is_anonymous(vma)` -> `CRIU_VMA_ANON_PRIVATE`.
3. `VM_SHARED` + `vm_file` + `inode->i_flags & S_PRIVATE` ->
   `CRIU_VMA_ANON_SHARED`. This is the path created by 5.10's
   `shmem_zero_setup()` for `MAP_SHARED|MAP_ANONYMOUS`.
4. `VM_SHARED` + `vm_file` -> `CRIU_VMA_FILE_SHARED`.
5. `vm_file` -> `CRIU_VMA_FILE_PRIVATE`.
6. Anything else -> `CRIU_VMA_UNSUPPORTED`.

The fixture must include both `MAP_SHARED|MAP_ANONYMOUS` and an explicit
tmpfs/memfd `MAP_SHARED` mapping so this distinction is tested rather than inferred
from `vm_file == NULL`.

## Internal data contract

The public A1 walk interface passes value snapshots rather than raw
`struct vm_area_struct *` pointers:

```c
enum criu_vma_class {
	CRIU_VMA_ANON_PRIVATE,
	CRIU_VMA_ANON_SHARED,
	CRIU_VMA_FILE_SHARED,
	CRIU_VMA_FILE_PRIVATE,
	CRIU_VMA_UNSUPPORTED,
};

enum criu_vma_special {
	CRIU_VMA_SPECIAL_NONE,
	CRIU_VMA_SPECIAL_PROT_NONE,
	CRIU_VMA_SPECIAL_VDSO,
	CRIU_VMA_SPECIAL_VVAR,
	CRIU_VMA_SPECIAL_HUGETLB,
	CRIU_VMA_SPECIAL_DEVICE,
	CRIU_VMA_SPECIAL_PFNMAP,
	CRIU_VMA_SPECIAL_MIXEDMAP,
	CRIU_VMA_SPECIAL_UNKNOWN,
};

struct criu_vma_info {
	unsigned long start, end, pgoff;
	unsigned long vm_flags_raw;
	unsigned int prot;
	enum criu_vma_class class;
	enum criu_vma_special special;
	bool growsdown, dontdump, locked;
	dev_t dev;
	unsigned long ino;
	char path[CRIU_PATH_MAX];
};

struct criu_mm_info {
	pid_t pid, tgid;
	char comm[TASK_COMM_LEN];
	unsigned long state;
	unsigned long vma_count, special_count, unsupported_count;
	unsigned long total_vm;
	unsigned long start_code, end_code;
	unsigned long start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end;
	unsigned long env_start, env_end;
};

typedef int (*criu_vma_info_fn)(const struct criu_vma_info *info,
				void *arg);

int criu_collect_mm_info(struct task_struct *task,
				 struct criu_mm_info *out);
int criu_walk_vmas(struct task_struct *task,
			   criu_vma_info_fn fn, void *arg);
```

The raw VMA pointer remains private to `vma_walk.c`. Page reads are a separate
helper and are not part of the A1 metadata callback.

## Debugfs control and lifetime model

The module exposes:

```text
/sys/kernel/debug/criu/target
/sys/kernel/debug/criu/task
/sys/kernel/debug/criu/maps
/sys/kernel/debug/criu/vmas_ext
```

Global target state is protected by a mutex and contains a pinned task reference
and a monotonically increasing `generation`. Writing `target` resolves and pins a
task before atomically replacing the old target; a failed lookup leaves the old
target unchanged. Each `open()` captures the task reference and generation in its
`seq_file` private data, so later target replacement cannot change an in-flight
read. `release()` drops the reference.

`get_task_mm()` failure is reported as `-ESRCH`, including kernel threads and tasks
that have completed `exit_mm()`. PID reuse is detected by the pinned task pointer
and generation, not by the numeric PID alone.

## Output contract

`maps` keeps the `/proc/PID/maps` field order:

```text
start-end perms offset dev inode path
```

Addresses and offsets are lower-case hexadecimal. Missing paths use `-`.

`task` and `vmas_ext` include `generation`, `pid` and `tgid`. `vmas_ext` uses
fixed `key=value` fields, retaining raw `vm_flags` for diagnostics while exposing
normalized `prot`, `class`, `special`, `dump_policy`, backing identity and path
status. The text format is for inspection and tests only; it is not the future
CRIU image format.

## Special VMA policy

A1 reports every VMA and continues the walk:

| VMA | A1 result | Page policy | A3 result |
|---|---|---|---|
| ordinary four classes | normalized snapshot | eligible | supported |
| `PROT_NONE` | metadata | skip | preserve VMA, no page bytes |
| `VM_DONTDUMP` | `dump_policy=SKIP` | skip | apply CRIU rule |
| vDSO/vvar | special or `UNKNOWN` | no ordinary read | CRIU-specific handling |
| hugetlb | `HUGETLB` | skip | unsupported in A3 |
| IO/PFN/MIXEDMAP | special | skip | reject in A3 |
| unknown combination | `UNKNOWN` | skip | reject in A3 |

The A1 gate checks recognition and reporting. A3 separately requires
`unsupported_count == 0` for its minimal fixture.

## Test fixture and acceptance

Create `tests/progs/a1-layout.c` with:

- ELF/file-private mappings;
- private anonymous memory containing `0xa5`;
- shared anonymous memory containing `0x5a`;
- explicit tmpfs or memfd shared memory containing `0x3c`;
- a `PROT_NONE` guard region;
- an `mprotect()` operation that splits a VMA.

The A1 comparison gate must cover:

- exact `maps` comparison against `/proc/PID/maps`;
- task/mm summary consistency and VMA counts;
- all four ordinary classes and magic bytes;
- special-VMA reporting and page-skip behavior;
- 2000+ VMA traversal;
- nonexistent PID, kernel thread and target-exit races;
- target generation and PID reuse;
- `CAP_SYS_ADMIN` rejection;
- repeated module load/unload without residual state;
- clean dmesg with DEBUG_VM, PROVE_LOCKING and KASAN;
- checkpatch and sparse checks.

## Approval boundary

This document freezes the A1 design. The next artifact is an implementation plan
with atomic tasks, file ownership, dependencies and verification commands. No A1
source implementation begins until that plan is reviewed.
