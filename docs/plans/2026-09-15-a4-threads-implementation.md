# A4 多线程 Dump Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend the A3 kernel dump from one thread to a frozen thread group, emitting one
CRIU-compatible `core-$tid.img` per thread and a complete `pstree.img` thread list.

**Architecture:** Keep A3's process-wide dump one-per-thread-group for mm, pages, files,
and credentials. Add a two-phase thread snapshot: pin every thread under RCU, release
the read-side section, then perform sleeping image I/O from the pinned task references.
The converter consumes one process record plus repeated per-thread records and emits the
leader and non-leader core images without changing the A3 image contract for single-thread
processes.

**Tech Stack:** Linux 5.10.29/aarch64, GPL out-of-tree kernel module, CRIU protobuf image
format, protobuf-compatible userspace converter, Lima + nested QEMU guest tests, shell
contract tests, and real `criu restore`.

---

## Scope and non-goals

- Include all tasks in the selected thread group, including the group leader.
- Dump per-thread TID, registers, TLS, signal mask, and thread-local metadata needed by
  the existing CRIU `core.proto` mapping.
- Keep mm/VMA/pages, files, fs, credentials, and shared process metadata single-copy.
- Preserve the A3 single-thread image layout and acceptance tests.
- Do not implement process descendants, per-thread pending-signal image families, POSIX
  timers, or shared resources here; those belong to A6, A7, and A8.
- Do not use `/proc` text parsing in the kernel module.

## Acceptance gates

1. A2 freeze status reports and settles every thread in the selected group.
2. The A4 snapshot contains exactly the frozen thread set, with no duplicate or missing TID.
3. The converter emits `core-$leader.img` and one `core-$tid.img` for every non-leader.
4. `pstree.img` contains the same TID set as the core image set.
5. A3 single-thread `cross-restore.sh`, converter tests, and field checks remain passing.
6. The multithread fixture passes real CRIU restore in the Linux 5.10.29 QEMU guest, or
   reports a documented unsupported field rather than producing a partial image.

### Task 1: Add a failing thread-group contract test

**Files:**
- Create: `tests/a4-thread-contract.sh`

**Steps:**

1. Assert that the kernel dump sources expose a thread snapshot type and a two-phase
   enumeration path.
2. Assert that the converter has a per-thread record parser and emits a `core-$tid.img`
   name from the record TID.
3. Assert that no thread callback performs image I/O while an RCU read-side section is held.
4. Run the contract before implementation and record the expected failure.
5. Commit the test separately.

### Task 2: Extend the snapshot ABI with per-thread records

**Files:**
- Modify: `include/criu_snapshot.h`
- Modify: `kernel_module/include/criu_kernel.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `userspace/criu-module-convert/criu_model.c`

**Steps:**

1. Add a versioned per-thread record containing virtual TID, thread-group ID, register blob,
   TLS value, blocked signal mask, and thread-local fields required by the converter.
2. Keep the existing process record as the leader/process-wide record; do not duplicate mm,
   files, fs, or credentials in every thread record.
3. Reject truncated, duplicate, or leader-mismatch thread records.
4. Add fixture coverage for a leader-only document and a leader plus two worker records.
5. Run `make -C userspace` and converter format tests.
6. Commit the ABI and reader changes.

### Task 3: Implement safe thread enumeration and pinning

**Files:**
- Create: `kernel_module/checkpoint/dump_threads.c`
- Create: `kernel_module/checkpoint/dump_threads.h`
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `kernel_module/Makefile`

**Steps:**

1. Under RCU, count the complete thread group with `for_each_thread()` and include the
   leader exactly once.
2. Allocate an array of pinned task references and copy each virtual TID while RCU is held.
3. Take a task reference for every member, release RCU, and revalidate the count before
   writing image data.
4. Return `-EAGAIN` or `-ESRCH` on a thread exit/race; never dereference an unpinned task
   after leaving RCU.
5. Release every task reference on all error paths.
6. Build the module against the Linux 5.10.29 guest kernel.
7. Commit the enumeration component.

### Task 4: Capture and serialize per-thread CPU state

**Files:**
- Modify: `kernel_module/checkpoint/dump_task.c`
- Modify: `kernel_module/checkpoint/dump_threads.c`
- Modify: `kernel_module/checkpoint/dump_threads.h`

**Steps:**

1. Generalize A3 register capture to every pinned task.
2. Capture `task_pt_regs(thread)` only after A2 reports the thread as settled.
3. On aarch64, capture `thread.uw.tp_value` as the thread TLS value.
4. Copy the per-thread blocked signal mask and task-local fields required by the snapshot ABI.
5. Keep pending queues and timers out of A4; do not silently serialize incomplete A6 state.
6. Test allocation and cleanup with 1, 2, and 8-thread fixtures.
7. Commit the capture implementation.

### Task 5: Emit CRIU core images for every thread

**Files:**
- Modify: `userspace/criu-module-convert/criu_model.c`
- Modify: `tests/converter-images.sh`
- Modify: `tests/criu-field-compare.sh`

**Steps:**

1. Build the leader core using process-wide `task_core_entry` plus its thread entry.
2. Build each non-leader core using the per-thread entry and architecture register entry.
3. Update `pstree.img` with the exact ordered TID list.
4. Validate that every TID appears once in `pstree.img` and has one core image.
5. Keep A3 single-thread output compatible where semantics are unchanged.
6. Run converter generation and `crit decode` structural checks.
7. Commit the converter/image changes.

### Task 6: Add a real multithread fixture and guest gate

**Files:**
- Create or adapt: `tests/progs/threads.c`
- Create: `tests/a4-cross-restore.sh`

**Steps:**

1. Build an aarch64 static fixture with eight pthreads, independent TLS values, stack
   patterns, and monotonically increasing per-thread counters.
2. Dump it through the existing debugfs/shim path while A2 freezes the complete group.
3. Verify the thread count, TID set, TLS fields, and core image set after conversion.
4. Run real `criu restore` with a bounded timeout.
5. Verify each restored thread remains alive and reports its own TLS/counter identity.
6. Classify failures as A4 data loss, deferred A6/A7 state, or environment failure.
7. Commit the fixture and gate.

### Task 7: Regression and documentation gate

**Files:**
- Modify: `docs/steps/A4-threads.md`
- Modify: `docs/03-Iteration-Plan.md`
- Modify: `ci/zdtm-allowlist.txt` only for individually verified tests

**Steps:**

1. Run `sh -n` on changed shell scripts and `git diff --check`.
2. Run A3 converter/image tests and the full A2/A3 QEMU smoke gates.
3. Run the A4 guest gate in Lima plus nested QEMU.
4. Add only stable pthread/TLS ZDTM cases to the allowlist, with a reason for each.
5. Record unsupported per-thread signal/timer behavior for A6.
6. Update the completion checklist with exact commands and commit the final A4 documentation.

## Verification commands

```sh
make -C userspace
sh -n tests/a4-thread-contract.sh tests/a4-cross-restore.sh
git diff --check
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/a4-cross-restore.sh'
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh'
```

## Deferred work

- A9 detailed cgroup/namespace/mount/fs-context design.
- A10 complete support/拒绝 matrix and release gate.
- B1/B2 user-space restore.
- C/X kernel-assisted restore experiments.
