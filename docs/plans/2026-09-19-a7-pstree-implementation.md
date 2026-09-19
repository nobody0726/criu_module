# A7 进程树、Session 与进程组 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the A2 single-thread-group dump into a same-PID-namespace descendant closure, preserve process/session/process-group topology, and produce a CRIU-compatible multi-process image set with real Linux 5.10.29 guest restore evidence.

**Architecture:** A7 adds one immutable process/task closure to the A2 freeze context. A patched Linux 5.10.29 cgroup wrapper freezes all selected process leaders in one temporary freezer cgroup; kernel collectors consume only the pinned closure and emit little-endian process-scoped TLV records. The userspace converter validates the complete topology and owner graph before generating one CRIU `pstree.img` plus per-process/per-thread images. Legacy `include_children=false` snapshots remain byte-compatible.

**Tech Stack:** Linux 5.10.29/aarch64 kernel module, patched cgroup-v2 freezer wrapper, C11 userspace converter, CRIU protobuf wire images, Lima `criu-dev`, nested QEMU guest, shell/Python contract fixtures, static C process-tree fixtures, and real `criu restore`.

**Spec:** `docs/plans/2026-09-19-a7-pstree-design.md`

## Global Constraints

- Target kernel is Linux 5.10.29; do not use 6.1+ maple-tree APIs.
- Architecture is aarch64; all guest behavior gates run in nested Linux 5.10.29/aarch64 QEMU.
- macOS only edits/orchestrates; Lima builds Linux artifacts; only the nested guest may `insmod`, `rmmod`, or execute kernel dump behavior.
- Do not call `kallsyms_lookup_name()` or bypass the A2 patched wrapper ABI.
- Kernel emits versioned little-endian TLV data only; userspace emits CRIU protobuf images.
- A7 uses one frozen closure; collectors must not call `criu_target_get()` or independently re-enumerate descendants/threads on the A7 path.
- No file I/O, sleeping allocation, or writer call while holding tasklist/RCU/signal/timer/cgroup spinlock state.
- Any failure aborts the whole snapshot, removes temporary output, thaws every selected process, and releases every task/namespace/cgroup reference.
- A7 first gate rejects cross-PID-namespace targets, missing session/process-group leaders, TASK_HELPER topologies, cross-process A8 shared objects, and zombie restore semantics.
- A7 first gate requires all selected process leaders to share one original cgroup-v2 path; multiple original paths are `UNSUPPORTED` until the freezer cookie ABI can restore them individually.
- A3 retrospective rules apply: restore exit status is insufficient; every guest gate checks restored PID liveness, relationship, and behavior markers.
- Do not add generated binaries, guest logs, `artifacts/`, or temporary image directories to Git.
- Existing A3-A6 `include_children=false` behavior and legacy flags=0 snapshot parsing must remain passing.

## Plan-wide Interfaces

The following names are the cross-task contract. Later tasks must use these exact names unless a compiler or Linux 5.10.29 header requires an explicitly documented equivalent.

```c
struct criu_freeze_process_view {
	struct task_struct *leader;
	struct task_struct *parent;
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	bool root;
	bool external_parent;
	bool session_leader;
	bool process_group_leader;
	unsigned int task_count;
};

int criu_freeze_process_count(struct criu_freeze_ctx *ctx,
				      unsigned int *count);
int criu_freeze_process_get(struct criu_freeze_ctx *ctx,
				   unsigned int process_index,
				   struct criu_freeze_process_view *view);
int criu_freeze_process_task_count(struct criu_freeze_ctx *ctx,
					   unsigned int process_index,
					   unsigned int *count);
int criu_freeze_process_task_get(struct criu_freeze_ctx *ctx,
					 unsigned int process_index,
					 unsigned int task_index,
					 struct criu_freeze_task_view *view);
```

For the snapshot writer, add one owner-aware helper while preserving the existing ABI for legacy callers:

```c
int criu_snapshot_writer_process_record(struct criu_snapshot_writer *writer,
						uint32_t owner_pid, uint16_t type,
						uint16_t flags, const void *payload,
						size_t length);
```

The helper sets `CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE` and prepends the 8-byte `owner_pid/reserved` prefix. Global `PSTREE` records continue to use `criu_snapshot_writer_record()`.

---

## Task 1: Lock the A7 snapshot ABI and malformed fixtures

**Files:**
- Modify: `include/criu_snapshot.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `userspace/criu-module-convert/snapshot_reader.h`
- Create: `tests/a7-abi-contract.sh`
- Create: `tests/fixtures/a7-snapshot-builder.py`

**Interfaces:**
- Consumes: existing A3-A6 record constants and snapshot reader status codes.
- Produces: `CRIU_SNAPSHOT_F_PSTREE`, `CRIU_SNAPSHOT_REC_PSTREE`, `CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE`, packed A7 structs, and fixture builders used by Tasks 2, 5, and 6.

- [ ] **Step 1: Write the failing ABI contract.**

Add checks for the new header flag, record type 19, process-scope flag, fixed sizes, topology flags, and little-endian packed structs. Add fixture cases for valid simple/session/pgid trees and malformed duplicate PID, missing parent, missing leader, owner mismatch, cross-namespace marker, and `born_sid` conflict.

- [ ] **Step 2: Run the contract before implementation.**

Run in the Lima build environment:

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   sh tests/a7-abi-contract.sh'
```

Expected before the ABI implementation: a targeted failure naming the missing A7 constant or packed-size assertion.

- [ ] **Step 3: Add the packed ABI definitions.**

Add:

```c
#define CRIU_SNAPSHOT_F_PSTREE (1U << 1)
#define CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE (1U << 0)
#define CRIU_SNAPSHOT_REC_PSTREE 19
#define CRIU_SNAPSHOT_PSTREE_VERSION 1U
#define CRIU_SNAPSHOT_PSTREE_RECORD_SIZE 48U
#define CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE 8U
```

Define the 48-byte `criu_snapshot_pstree_record` and 8-byte `criu_snapshot_process_scope` with explicit little-endian integer fields. Add flags for root, external parent, session leader, and process-group leader. Extend known-header and known-record validation without changing legacy flags=0 behavior.

- [ ] **Step 4: Extend reader validation.**

When `CRIU_SNAPSHOT_F_PSTREE` is absent, continue accepting A3-A6 legacy snapshots. When present, require `PSTREE` records and reject unknown header flags, short records, unsupported namespace scope, and process-scoped records shorter than the owner prefix. Preserve the existing distinction between `CRIU_SNAPSHOT_FORMAT_ERROR` and `CRIU_SNAPSHOT_UNSUPPORTED`.

- [ ] **Step 5: Complete fixture generation and run the contract.**

The Python builder must emit deterministic records sorted by PID and include enough TASK/THREAD records for converter tests. Run:

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   sh tests/a7-abi-contract.sh'
```

Expected: `A7_ABI_CONTRACT: PASS`; malformed fixtures must fail before output publication.

- [ ] **Step 6: Commit.**

```sh
git add include/criu_snapshot.h userspace/criu-module-convert/snapshot_reader.* \
  tests/a7-abi-contract.sh tests/fixtures/a7-snapshot-builder.py
git commit -m "test: define A7 process tree snapshot ABI"
```

## Task 2: Add the process-set cgroup freezer wrapper

**Files:**
- Modify: `patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch`
- Modify: `include/linux/criu_freezer.h`
- Modify: `kernel_module/include/criu_freezer.h`
- Create: `tests/a7-freezer-wrapper-contract.sh`
- Modify: `scripts/apply-kernel-patches.sh`

**Interfaces:**
- Consumes: A2 single-thread-group wrapper semantics and Linux 5.10.29 cgroup internals.
- Produces: exported `criu_cgroup_freeze_process_set()` and matching cookie-based thaw behavior consumed by Task 3.

- [ ] **Step 1: Add the failing wrapper contract.**

Check that the patch declares the process-set prototype, protects all leaders with `cgroup_threadgroup_change_begin/end`, attaches every leader to one temporary cgroup, freezes once, rolls back partial attachment, and exports both freeze and thaw symbols. Reject module-side direct calls to `cgroup_attach_task()` and `cgroup_freeze()`.

- [ ] **Step 2: Verify the current patch fails the new contract.**

Run:

```sh
sh tests/a7-freezer-wrapper-contract.sh
```

Expected before implementation: FAIL because only `criu_cgroup_freeze_threadgroup()` exists.

- [ ] **Step 3: Implement process-set wrapper logic inside kernel core.**

Keep all cgroup internals in the patched kernel source. Validate non-null leaders, non-zero count, every leader is its own `group_leader`, and `*cookie == NULL`. Under the existing cgroup mutex/threadgroup protection, create one temporary child, attach each leader with the wrapper's complete-threadgroup operation, and remember how many leaders succeeded. On failure, thaw/detach already attached leaders in reverse order, destroy the temporary cgroup, release references, and return the original error. On thaw, unfreeze once, restore every original cgroup membership, and retain the cookie if any restore or destroy step fails.

- [ ] **Step 4: Apply and build the patched Linux kernel.**

Run the project patch script and verify the exported symbols from the Linux 5.10.29 tree inside Lima:

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/apply-kernel-patches.sh && \
   grep -q "EXPORT_SYMBOL_GPL(criu_cgroup_freeze_process_set)" \
     /home/yhome.guest/kernels/linux-5.10.29/kernel/cgroup/cgroup.c'
```

- [ ] **Step 5: Run the wrapper contract and commit.**

```sh
sh tests/a7-freezer-wrapper-contract.sh
git add patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch \
  include/linux/criu_freezer.h kernel_module/include/criu_freezer.h \
  tests/a7-freezer-wrapper-contract.sh scripts/apply-kernel-patches.sh
git commit -m "feat: add process-set cgroup freezer wrapper"
```

## Task 3: Extend A2 freeze context with an immutable process closure

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Modify: `kernel_module/core/main.c`
- Modify: `kernel_module/Makefile`
- Create: `kernel_module/checkpoint/collect_tree.c`
- Create: `kernel_module/checkpoint/collect_tree.h`
- Create: `tests/a7-tree-contract.sh`

**Interfaces:**
- Consumes: Task 2 process-set wrapper; existing A2 target generation and thread pinning.
- Produces: process/task accessors from the Plan-wide Interfaces and an immutable closure used by Tasks 4-6.

- [ ] **Step 1: Write contract checks for closure ownership.**

The contract must require an explicit-stack/queue traversal, namespace binding, process/task pinning, `criu_freeze_process_*` accessors, and no `criu_target_get()` or `for_each_process()` call from A7 collector code after freeze. It must reject recursive tree traversal and writer calls inside RCU/tasklist critical sections.

- [ ] **Step 2: Add internal closure structures.**

In `collect_tree.c`, define private nodes containing leader/parent references, PID namespace reference, virtual IDs, `born_sid`, root/leader flags, and a separately pinned array of thread references. Keep ownership in `criu_freeze_ctx`; public views are borrowed and invalid after thaw.

- [ ] **Step 3: Implement explicit-stack descendant discovery.**

Bind the target's active PID namespace and compare it with the control caller namespace. Walk `children` under the Linux 5.10.29 tasklist/RCU rules, enqueue each descendant leader once, and skip tasks in another PID namespace. Reject a namespace mismatch rather than translating a partial tree. Detect loops, duplicate leaders, exiting tasks, and a process count above the documented bounded limit.

- [ ] **Step 4: Pin every process and thread.**

For each discovered leader, enumerate the complete thread group once under RCU, take task references, copy TIDs, and release RCU before allocations or I/O. Recheck the process and thread counts after cgroup freeze; return `-EAGAIN` when the closure changed and `-ESRCH` when a member exited.

- [ ] **Step 5: Compute topology fields.**

While all references are held, compute `pid/ppid/pgid/sid` using the bound namespace (`pid_nr_ns()` and namespace-aware pgrp/session helpers where available), mark root/external-parent/session-leader/process-group-leader, and derive `born_sid` with the CRIU rule. Reject missing leaders and conflicting born-session requirements before opening the snapshot writer.

- [ ] **Step 6: Add process-set freeze/thaw and legacy branching.**

Keep `include_children=false` on the current A2 path. For `include_children=true`, collect the closure, call the process-set wrapper exactly once, settle all pinned tasks, and expose the new accessors. Ensure every rollback path calls the matching process-set thaw and releases all nodes, threads, and namespace references.

- [ ] **Step 7: Build and run contracts.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29 && \
   sh tests/a7-tree-contract.sh'
```

Expected: module build succeeds; `A7_TREE_CONTRACT: PASS`; existing A2 freeze tests still pass on the legacy path.

- [ ] **Step 8: Commit.**

```sh
git add kernel_module/checkpoint/freeze.c kernel_module/checkpoint/collect_tree.* \
  kernel_module/include/criu_kernel.h kernel_module/core/main.c \
  kernel_module/Makefile tests/a7-tree-contract.sh
git commit -m "feat: freeze and expose A7 process closure"
```

## Task 4: Serialize PSTREE and process-scoped records transactionally

**Files:**
- Modify: `kernel_module/checkpoint/snapshot_writer.c`
- Modify: `kernel_module/checkpoint/snapshot_writer.h`
- Create: `kernel_module/checkpoint/dump_pstree.c`
- Create: `kernel_module/checkpoint/dump_pstree.h`
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `kernel_module/core/main.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Modify: `tests/dump-errors.sh`
- Create: `tests/a7-dump-transaction.sh`

**Interfaces:**
- Consumes: validated immutable closure and process-scoped ABI from Tasks 1-3.
- Produces: global PSTREE records, owner-prefixed process records, atomic A7 snapshot publication.

- [ ] **Step 1: Add failing transaction tests.**

Force a missing leader, writer failure, and collector failure. Assert no final snapshot, no `.tmp`, no stale process-set cookie, and that every target process can be selected/frozen again afterward.

- [ ] **Step 2: Implement owner-aware writer helper.**

Add `criu_snapshot_writer_process_record()` that allocates an 8-byte owner prefix in temporary memory, sets `CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE`, and delegates to the existing writer. Keep the existing writer unchanged for global records and legacy paths. Never hold tasklist/RCU locks while calling it.

- [ ] **Step 3: Implement PSTREE serialization.**

Write one global PSTREE record per process sorted by virtual PID. Validate the entire closure before the first writer open. Encode root/external/leader flags and signed `born_sid` in the fixed 48-byte payload. Do not write a partial tree if any node fails validation.

- [ ] **Step 4: Add the `/sys/kernel/debug/criu/dump-tree` transaction path.**

Add an explicit `criu_dump_process_tree()` entry and a debugfs file named `dump-tree`. Its write format is exactly `<root_pid> <snapshot_path>`, matching the existing `dump` file. The handler must require `CAP_SYS_ADMIN`, parse the same bounded input size, set `CRIU_SNAPSHOT_F_PSTREE`, reject a closure whose leaders have different original cgroup paths, freeze with `include_children=true`, emit PSTREE first, then iterate process nodes in PID order and call process-aware collectors. Existing single-process `criu_dump_process()` remains unchanged except for shared helper extraction.

- [ ] **Step 5: Revalidate before commit.**

Compare generation, namespace, process count, PID set, parent IDs, thread counts, and leader references against the immutable closure. Any mismatch returns `-EAGAIN` (or `-EINVAL` for a permanently malformed topology) and aborts the writer before `REC_END`.

- [ ] **Step 6: Run transaction and existing error gates.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   sh tests/a7-dump-transaction.sh && \
   sh tests/dump-errors.sh'
```

Expected: `A7_DUMP_TRANSACTION: PASS` and existing `DUMP_ERRORS: PASS`.

- [ ] **Step 7: Commit.**

```sh
git add kernel_module/checkpoint/snapshot_writer.* kernel_module/checkpoint/dump_pstree.* \
  kernel_module/checkpoint/dump.c kernel_module/include/criu_kernel.h \
  tests/dump-errors.sh tests/a7-dump-transaction.sh
git commit -m "feat: serialize A7 process tree transactionally"
```

## Task 5: Migrate A3-A6 collectors to explicit process views

**Files:**
- Modify: `kernel_module/checkpoint/dump_task.c`
- Modify: `kernel_module/checkpoint/dump_threads.c`
- Modify: `kernel_module/checkpoint/dump_mm.c`
- Modify: `kernel_module/checkpoint/page_scan.c`
- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `kernel_module/checkpoint/dump_signals.c`
- Modify: `kernel_module/checkpoint/dump_timers.c`
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `tests/dump-task-contract.sh`
- Modify: `tests/a4-thread-contract.sh`
- Modify: `tests/a5-fd-contract.sh`
- Modify: `tests/a6-abi-contract.sh`

**Interfaces:**
- Consumes: Task 3 closure accessors and Task 4 owner-aware writer.
- Produces: process-scoped TASK/THREAD/MM/VMA/PAGE/FD/FS/A6 records with no independent target enumeration.

- [ ] **Step 1: Add failing source contracts.**

Require A7 collector entry points to accept a `criu_freeze_process_view` or explicit task reference plus owner PID. Reject calls to `criu_target_get()` and unscoped writer calls on the A7 path. Assert all RCU/tasklist sections end before writer I/O.

- [ ] **Step 2: Generalize process-wide TASK and thread records.**

Change dump functions to accept the frozen process view. Use namespace-bound PID values, owner-prefixed process records, and the process's pinned thread list. Keep the old single-process function as a compatibility wrapper that constructs the legacy unscoped record path.

- [ ] **Step 3: Generalize MM/pages/files.**

Pass the process leader reference from the closure into VMA/page/FD/FS collectors. Prefix every A7 record with the owner PID. Preserve existing object IDs within one process and reject cross-process object sharing before any object is emitted.

- [ ] **Step 4: Generalize A6 collectors.**

Use the process view's task list for blocked masks, private queues, POSIX timer targets, and signal ownership. Shared queue/sigaction/timer records receive the process owner prefix; `notify_tid` must resolve to a thread in the same closure. Do not re-enumerate a thread group.

- [ ] **Step 5: Add cross-process sharing rejection.**

Compare `mm_struct`, `files_struct`, and relevant A5 object identities across process nodes. Return `-EOPNOTSUPP` for non-thread `CLONE_VM`, external `CLONE_FILES`, cross-process pipe/socket sharing, or any SysV SHM reference that would require A8 deduplication.

- [ ] **Step 6: Build and run source contracts.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29 && \
   sh tests/dump-task-contract.sh && \
   sh tests/a4-thread-contract.sh && \
   sh tests/a5-fd-contract.sh && \
   sh tests/a6-abi-contract.sh'
```

Expected: all legacy contracts remain PASS and new A7 process-scoped assertions pass.

- [ ] **Step 7: Commit.**

```sh
git add kernel_module/checkpoint/dump_task.c kernel_module/checkpoint/dump_threads.c \
  kernel_module/checkpoint/dump_mm.c kernel_module/checkpoint/page_scan.c \
  kernel_module/checkpoint/dump_files.c kernel_module/checkpoint/dump_signals.c \
  kernel_module/checkpoint/dump_timers.c kernel_module/checkpoint/dump.c \
  tests/dump-task-contract.sh tests/a4-thread-contract.sh tests/a5-fd-contract.sh \
  tests/a6-abi-contract.sh
git commit -m "feat: scope A3-A6 collectors to frozen processes"
```

## Task 6: Extend converter model and generate multi-process CRIU images

**Files:**
- Modify: `userspace/criu-module-convert/criu_model.h`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `tests/fixtures/a7-snapshot-builder.py`
- Create: `tests/a7-converter-images.sh`
- Create: `tests/a7-unsupported.sh`

**Interfaces:**
- Consumes: Task 1 A7 ABI and Task 4 owner-prefixed records.
- Produces: validated `pstree_table`, per-process model index, topology diagnostics, and CRIU image directory.

- [ ] **Step 1: Add failing converter assertions.**

Feed a valid multi-process fixture and assert the current single-process converter rejects/ignores it. Add negative cases for duplicate PID, orphan parent, missing leaders, owner mismatch, duplicate thread, A8 sharing, and unsupported namespace.

- [ ] **Step 2: Add model structures and indexed parsing.**

Introduce `pstree_table`, `process_model`, and owner-indexed blob lists. Parse the owner prefix only when `CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE` is set. Reject a scoped record whose owner PID is zero or whose payload is shorter than the prefix. Keep the existing legacy model path for flags=0.

- [ ] **Step 3: Implement complete topology validation.**

Validate one root, unique PIDs, parent closure, no cycles, `pid==tgid==leader_pid`, session/group leader presence, namespace scope, `born_sid` range/conflicts, thread ownership/completeness, A6 target TIDs, and A8 sharing markers. Return `CRIU_SNAPSHOT_UNSUPPORTED` for unsupported capability and `CRIU_SNAPSHOT_INCONSISTENT` for contradictory data.

- [ ] **Step 4: Build one CRIU pstree entry per process.**

Emit `pid`, `ppid`, `pgid`, `sid`, and ordered `threads` exactly from the validated process model. Do not emit `born_sid` as a non-standard protobuf field. If CRIU-equivalent pre-`setsid()` analysis detects helper-required or ambiguous topology, fail before creating the output directory.

- [ ] **Step 5: Emit process-scoped image families.**

Generate `core-$pid.img`, `mm-$pid.img`, `fs-$pid.img`, `creds-$pid.img`, and each `core-$tid.img` from the matching process model. Build closure-wide `files.img`/`reg-files.img`/`fdinfo` only after all owners and object references validate. Do not reuse root data for another PID.

- [ ] **Step 6: Preserve atomic publication and legacy output.**

Validate the entire model before `mkdir`/image creation. Write to a staging directory, remove it on any failure, and atomically publish only after all images succeed. Run the existing A3-A6 converter tests to prove flags=0 output is unchanged.

- [ ] **Step 7: Run converter gates.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   make -C userspace/criu-module-convert clean all LDFLAGS= && \
   sh tests/a7-converter-images.sh && \
   sh tests/a7-unsupported.sh && \
   sh tests/a6-converter-images.sh && \
   sh tests/a5-converter-fds.sh'
```

Expected: `A7_CONVERTER_IMAGES: PASS`, `A7_UNSUPPORTED: PASS`, and A5/A6 converter regressions PASS.

- [ ] **Step 8: Commit.**

```sh
git add userspace/criu-module-convert/criu_model.* userspace/criu-module-convert/snapshot_reader.c \
  tests/fixtures/a7-snapshot-builder.py tests/a7-converter-images.sh tests/a7-unsupported.sh
git commit -m "feat: generate multi-process CRIU images"
```

## Task 7: Add process-tree fixtures and guest-facing dump mode

**Files:**
- Create: `tests/progs/tree-simple.c`
- Create: `tests/progs/tree-session.c`
- Create: `tests/progs/tree-pgid.c`
- Create: `tests/progs/tree-invalid-leader.c`
- Create: `tests/progs/tree-fork-race.c`
- Create: `tests/progs/tree-shared-unsupported.c`
- Modify: `tests/progs/Makefile`
- Create: `tests/a7-cross-restore.sh`
- Modify: `tests/a7-unsupported.sh`

**Interfaces:**
- Consumes: Task 4 tree dump control path, Task 6 converter, existing `run-qemu.sh` and CRIU staging.
- Produces: deterministic simple/session/pgid behavior markers and explicit unsupported/race cases.

- [ ] **Step 1: Write fixture source and build rules.**

Each fixture prints a ready marker containing the root PID and expected descendant PIDs, uses only static aarch64-compatible libc/pthread APIs already used by the project, and writes behavior markers to a guest-local output file or inherited stdout. `tree-session` calls `setsid()` in the designated child; `tree-pgid` creates a group leader and joins a second child; invalid/shared/race fixtures expose one deterministic failure condition.

- [ ] **Step 2: Add failing guest script checks.**

The script must require a Linux 5.10.29 root guest, module/converter/CRIU availability, and `/sys/kernel/debug/criu/dump-tree`. Missing CRIU or guest prerequisites returns `A7_CROSS_RESTORE: SKIP: ENVIRONMENT`; it must never print PASS.

- [ ] **Step 3: Implement tree dump/convert/restore flow.**

For each case: launch fixture under `setsid`, wait for ready marker, set the root target, invoke tree dump to guest-local `/tmp`, verify final snapshot and no `.tmp`, run converter, kill the original closure, restore with bounded timeout, and capture restore log. Use `kill -0` for every expected restored PID before behavior checks.

- [ ] **Step 4: Verify topology and behavior after restore.**

Read `/proc/$pid/status` and `/proc/$pid/stat` in the guest to check `PPid`, process group, and session. Trigger the fixture's parent/child/group/session marker, then check all markers and liveness again. Reject any case where CRIU exits 0 but a PID disappears or a relationship/marker is wrong.

- [ ] **Step 5: Implement unsupported and race assertions.**

For missing leader, cross-namespace marker, A8 shared object, and fork-race cases, require explicit unsupported/inconsistent status, no final snapshot, no temporary output, a thawed target, and clean guest dmesg.

- [ ] **Step 6: Build fixtures in Lima.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   make -C tests/progs clean all'
```

- [ ] **Step 7: Commit.**

```sh
git add tests/progs/tree-*.c tests/progs/Makefile tests/a7-cross-restore.sh \
  tests/a7-unsupported.sh
git commit -m "test: add A7 process tree restore fixtures"
```

## Task 8: Run the nested guest A7 gate and classify diagnostics

**Files:**
- Modify: `tests/a7-cross-restore.sh` only for evidence/logging fixes
- Create: `docs/plans/2026-09-19-a7-verification.md`

**Interfaces:**
- Consumes: Tasks 1-7 complete ABI, module, converter, fixtures, and guest script.
- Produces: reproducible A7 gate evidence with environment/data/unsupported/failure classification.

- [ ] **Step 1: Build all artifacts in Lima.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29 && \
   make -C userspace/criu-module-convert clean all LDFLAGS= && \
   make -C tests/progs clean all'
```

- [ ] **Step 2: Run the authoritative guest command.**

Use the ARM64 CRIU checkout explicitly so a worktree without `criu/` cannot be mistaken for a passing restore:

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   CRIU_SOURCE=/Users/yhome/workspace/source_code/criu_module/criu \
   ./scripts/run-qemu.sh --ci --script tests/a7-cross-restore.sh'
```

- [ ] **Step 3: Require fresh PASS evidence.**

The evidence document must include:

```text
A7_SIMPLE: PASS
A7_SESSION: PASS
A7_PGID: PASS
A7_CROSS_RESTORE: PASS
```

plus guest kernel version, CRIU identity, module build identity, all host-side commands, and a dmesg scan with no BUG/Oops/WARNING/atomic-sleep/locking diagnostics. A missing CRIU, wrong kernel, or permission failure is recorded as `SKIP: ENVIRONMENT`, not PASS.

- [ ] **Step 4: Run unsupported and rollback cases.**

Run `tests/a7-unsupported.sh` in the same guest and record explicit `UNSUPPORTED`/`INCONSISTENT` output, absent final snapshots, absent `.tmp`, successful thaw, and clean diagnostics.

- [ ] **Step 5: Commit evidence.**

```sh
git add docs/plans/2026-09-19-a7-verification.md tests/a7-cross-restore.sh
git commit -m "docs: record A7 guest verification"
```

## Task 9: Full A3-A7 regression and documentation release gate

**Files:**
- Modify: `docs/steps/A7-pstree.md`
- Modify: `docs/03-Iteration-Plan.md`
- Modify: `docs/plans/2026-09-19-a7-pstree-design.md` for implementation-discovered clarifications
- No generated artifacts or logs

- [ ] **Step 1: Run shell and formatting checks.**

```sh
sh -n tests/a7-*.sh tests/dump-task-contract.sh
git diff --check
```

- [ ] **Step 2: Run host-side converter and contract matrix in Lima.**

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   sh tests/a7-abi-contract.sh && \
   sh tests/a7-freezer-wrapper-contract.sh && \
   sh tests/a7-tree-contract.sh && \
   sh tests/a7-converter-images.sh && \
   sh tests/a7-unsupported.sh && \
   sh tests/a6-abi-contract.sh && \
   sh tests/a6-converter-images.sh && \
   sh tests/a5-converter-fds.sh && \
   sh tests/dump-task-contract.sh'
```

- [ ] **Step 3: Run A3-A7 nested guest gates.**

Run A3, A4, A5, A6, and A7 scripts separately with explicit `CRIU_SOURCE`, and require each script's behavior/liveness marker. Do not replace this matrix with a single converter-only run.

- [ ] **Step 4: Run unsupported and cleanup checks.**

Confirm no `snapshot.bin`, `.tmp`, staging image directory, module reference, or freezer cookie remains after each negative case. Run `git status --short --untracked-files=all` and remove only generated fixture binaries/temp files; preserve user-created `artifacts/` and untracked planning documents.

- [ ] **Step 5: Update completion documents.**

Record A7's supported/unsupported matrix, process-set ABI, external root-parent limitation, `born_sid` validation, A8 boundary, guest command, and exact PASS/SKIP classification. Mark A7 complete only after fresh evidence satisfies the design document; keep TASK_HELPER, namespaces, A8, B2, full ZDTM, and GitHub CI in deferred scope.

- [ ] **Step 6: Commit the release gate.**

```sh
git add docs/steps/A7-pstree.md docs/03-Iteration-Plan.md \
  docs/plans/2026-09-19-a7-pstree-design.md
git commit -m "docs: complete A7 process tree release gate"
```

## Verification Matrix

| Layer | Command | Required result |
|---|---|---|
| ABI | `sh tests/a7-abi-contract.sh` in Lima | `A7_ABI_CONTRACT: PASS` |
| Wrapper | `sh tests/a7-freezer-wrapper-contract.sh` | `A7_FREEZER_WRAPPER: PASS` |
| Kernel tree | `sh tests/a7-tree-contract.sh` plus module build | `A7_TREE_CONTRACT: PASS` |
| Transaction | `sh tests/a7-dump-transaction.sh` | no partial snapshot and `A7_DUMP_TRANSACTION: PASS` |
| Converter | `sh tests/a7-converter-images.sh` | `A7_CONVERTER_IMAGES: PASS` |
| Unsupported | `sh tests/a7-unsupported.sh` | explicit unsupported/inconsistent and rollback PASS |
| Guest simple/session/pgid | `CRIU_SOURCE=... ./scripts/run-qemu.sh --ci --script tests/a7-cross-restore.sh` | all three behavior/liveness markers and `A7_CROSS_RESTORE: PASS` |
| Regression | A3-A6 existing gates | no legacy regression |

## Completion Criteria

A7 is complete only when:

- process-set freezer and immutable closure accessors build and work in Linux 5.10.29 guest;
- all process-scoped records are bound to the same closure and owner PID;
- topology validation rejects missing leaders, namespace mismatch, ambiguity, and A8 sharing before writer publication;
- converter emits standard CRIU multi-process images without non-standard protobuf fields;
- simple/session/pgid guest restore checks prove process liveness, parent/group/session relationships, and behavior;
- every negative path thaws and cleans up;
- A3-A6 gates remain PASS;
- verification documentation contains fresh reproducible evidence;
- deferred TASK_HELPER, namespace, A8, B2, full ZDTM, and GitHub CI work is recorded rather than silently claimed.
