# A1 Read-only Probe Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build the A1 kernel-module read layer that resolves one target PID, snapshots its task/mm/VMA metadata on Linux 5.10.29/aarch64, and verifies the result against procfs in a disposable QEMU guest.

**Architecture:** Replace the single-file probe with a small kernel module organized around target state, task lookup, VMA normalization, and debugfs views. The module pins a task reference and captures a generation per open; VMA callbacks receive value snapshots, while raw `vm_area_struct` pointers remain private to the walker. A user-space ARM64 fixture and guest comparison gate provide the acceptance oracle.

**Tech Stack:** Linux 5.10.29 kernel module, C, debugfs/seq_file, QEMU/initramfs via `scripts/run-qemu.sh`, shell tests, checkpatch, sparse.

---

## Dependencies and working rules

- Work only against the approved design in `docs/plans/2026-09-06-a1-readonly-probe-design.md`.
- All `insmod`, `rmmod`, and runtime VMA tests run inside disposable QEMU; never load the module on macOS or Lima directly.
- Preserve the untracked `artifacts/` directory; it contains prior S1 evidence and is not part of A1 commits.
- Use Linux kernel style and `/* ... */` comments only in C sources.
- Every task below ends with a focused verification and an atomic commit. Do not combine an unverified implementation with the next layer.

### Task 1: Establish A1 module layout and public types

**Files:**
- Create: `kernel_module/core/main.c`
- Create: `kernel_module/core/target.c`
- Create: `kernel_module/checkpoint/vma_walk.c`
- Create: `kernel_module/include/criu_kernel.h`
- Modify: `kernel_module/Makefile`
- Test: `tests/module-smoke.sh`

**Step 1: Write the compile gate**

Update `tests/module-smoke.sh` so it accepts the new module path
`kernel_module/criu_kernel.ko`, expects `/sys/kernel/debug/criu`, and still checks
load/status/unload. Keep a compatibility fallback only if needed while the old
`criu_probe.ko` is removed.

**Step 2: Run the gate to capture the pre-implementation failure**

Run: `sh -n tests/module-smoke.sh`

Expected: PASS for shell syntax; the guest runtime gate remains unavailable until
the new module exists. Do not treat host execution of `insmod` as a valid test.

**Step 3: Add the public enums and snapshot structs**

Define `enum criu_vma_class`, `enum criu_vma_special`, `struct criu_vma_info`,
`struct criu_mm_info`, `criu_vma_info_fn`, and the prototypes
`criu_collect_mm_info()` / `criu_walk_vmas()` exactly as approved. Keep
`vm_flags_raw` diagnostic-only and make `path` a bounded buffer.

**Step 4: Add the module skeleton and build objects**

Make `kernel_module/Makefile` build `criu_kernel.o` from `core/main.o`,
`core/target.o`, and `checkpoint/vma_walk.o`. Register a debugfs directory named
`criu`; the initial status file may report `criu_kernel:ok`.

Run: `make -C kernel_module KDIR=/path/to/linux-5.10.29`

Expected: exit 0 and `kernel_module/criu_kernel.ko` exists. If the kernel tree path
is not available on the host, run this command in the configured Linux build guest
and record the exact `KDIR` used.

**Step 5: Commit**

```bash
git add kernel_module tests/module-smoke.sh
git commit -m "feat: scaffold A1 kernel module" -m "Assisted-by: Codex: GPT-5"
```

### Task 2: Implement pinned target lookup and generation state

**Files:**
- Modify: `kernel_module/core/target.c`
- Modify: `kernel_module/core/main.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Test: `tests/compare/target-lifecycle.sh`

**Step 1: Write the failing lifecycle test**

Create a guest script that loads the module, writes a known live PID to `target`,
checks that `task` exposes `pid`, `tgid`, and `generation`, attempts a nonexistent
PID and verifies the old target remains, then unloads. Use a helper process that
prints its PID and pauses.

Run: `sh -n tests/compare/target-lifecycle.sh`

Expected: PASS for syntax; runtime fails because `target` is not implemented.

**Step 2: Implement target replacement**

Use a mutex-protected global state containing `struct task_struct *task` and `u64
generation`. Resolve with `find_vpid()` + `pid_task()` under `rcu_read_lock()`, call
`get_task_struct()` before unlocking, and only then replace the old target. On
lookup failure return `-ESRCH` without changing the old target. Increment generation
for every successful replacement, including a replacement with the same numeric PID
that resolves to a different task.

**Step 3: Bind references to seq_file opens**

At `open()` copy the current task reference and generation into private context;
release drops the task reference. All reads use that captured task, never re-read the
global pointer after open.

**Step 4: Add capability checks**

Reject target writes and all read/open operations unless `capable(CAP_SYS_ADMIN)`.
Return `-EPERM` and do not mutate state.

**Step 5: Run the focused guest test**

Run: `./scripts/run-qemu.sh --ci --script tests/compare/target-lifecycle.sh`

Expected: `A1_TARGET: PASS` and clean dmesg. A missing target or stale generation is
a failure, not a skip.

**Step 6: Commit**

```bash
git add kernel_module tests/compare/target-lifecycle.sh
git commit -m "feat: add pinned A1 target state" -m "Assisted-by: Codex: GPT-5"
```

### Task 3: Implement VMA normalization and mm summary

**Files:**
- Modify: `kernel_module/checkpoint/vma_walk.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Test: `tests/compare/vma-classifier-test.sh`

**Step 1: Extend the fixture contract before implementation**

Create the test-side parser and expected class names. It should require
`ANON_PRIVATE`, `ANON_SHARED`, `FILE_PRIVATE`, `FILE_SHARED`, and a summary line
with `unsupported_count`.

Run: `sh -n tests/compare/vma-classifier-test.sh`

Expected: syntax PASS; runtime fails because `vmas_ext` is not implemented.

**Step 2: Implement the classifier in the approved order**

Under the 5.10.29 headers, classify special flags first. Then use:

```c
if (vma_is_anonymous(vma))
	return CRIU_VMA_ANON_PRIVATE;
if ((vma->vm_flags & VM_SHARED) && vma->vm_file &&
    (file_inode(vma->vm_file)->i_flags & S_PRIVATE))
	return CRIU_VMA_ANON_SHARED;
if (vma->vm_file)
	return (vma->vm_flags & VM_SHARED) ?
		CRIU_VMA_FILE_SHARED : CRIU_VMA_FILE_PRIVATE;
return CRIU_VMA_UNSUPPORTED;
```

Do not call `vma_is_shmem()` from the module. Preserve the special classification
and `unsupported_count` rules from the approved design.

**Step 3: Implement `criu_walk_vmas()`**

Acquire `get_task_mm()`, return `-ESRCH` on NULL, hold `mmap_read_lock(mm)` for the
whole linked-list walk, populate a stack/value `criu_vma_info` per VMA, invoke the
callback, then unlock and `mmput()`. Never return a raw VMA pointer to callers.

**Step 4: Implement `criu_collect_mm_info()`**

Fill task identity and mm layout fields from the pinned task and mm. Count normal,
special, and unsupported VMAs during the same locked walk. Use bounded copies for
`comm` and diagnostic paths; record `path_status=TRUNCATED` when needed.

**Step 5: Run the focused guest test**

Run: `./scripts/run-qemu.sh --ci --script tests/compare/vma-classifier-test.sh`

Expected: `A1_CLASSIFIER: PASS`, all four classes present, and no dmesg warnings.

**Step 6: Commit**

```bash
git add kernel_module tests/compare/vma-classifier-test.sh
git commit -m "feat: normalize A1 VMA metadata" -m "Assisted-by: Codex: GPT-5"
```

### Task 4: Add the ARM64 A1 layout fixture

**Files:**
- Create: `tests/progs/a1-layout.c`
- Modify: `docs/steps/A1-readonly-probe.md`
- Test: `tests/progs/Makefile` (create if absent)

**Step 1: Write the fixture build contract**

Build a static ARM64 binary that creates private anonymous memory filled with
`0xa5`, shared anonymous memory filled with `0x5a`, an explicit tmpfs/memfd shared
mapping filled with `0x3c`, a `PROT_NONE` guard, and an `mprotect()`-split VMA. Print
PID and addresses, then call `pause()` forever.

Run: `sh -n tests/progs/Makefile` is not applicable to Make syntax; instead run the
cross-build command documented by the project.

Expected: a statically linked aarch64 ELF (`file` reports ARM aarch64 and static).

**Step 2: Verify the fixture independently**

Run inside the aarch64 guest: `./tests/progs/a1-layout > /tmp/a1-layout.out &`

Expected: one PID line, all mapping setup calls succeed, process remains alive, and
the printed magic addresses are page-aligned.

**Step 3: Commit**

```bash
git add tests/progs/a1-layout.c tests/progs/Makefile docs/steps/A1-readonly-probe.md
git commit -m "test: add A1 VMA layout fixture" -m "Assisted-by: Codex: GPT-5"
```

### Task 5: Implement task, maps, and extended debugfs views

**Files:**
- Modify: `kernel_module/core/main.c`
- Modify: `kernel_module/core/target.c`
- Modify: `kernel_module/checkpoint/vma_walk.c`
- Test: `tests/compare/diff-maps.sh`

**Step 1: Write the failing comparison gate**

Create `tests/compare/diff-maps.sh` to start `a1-layout`, set `target`, normalize
`/proc/$PID/maps` and module `maps` to start/end/perms/offset/dev/inode, compare
them with `diff -u`, then assert classes and magic bytes in `vmas_ext`.

Run: `sh -n tests/compare/diff-maps.sh`

Expected: syntax PASS; runtime fails until all three views are implemented.

**Step 2: Implement `maps`**

Render the captured VMA snapshots in procfs field order. Convert `vm_flags` to
`rwx` plus `p/s`; print lower-case hexadecimal addresses/offsets, numeric dev/inode,
and `-` for missing paths. Use `d_path()`'s returned pointer, not the beginning of
the buffer.

**Step 3: Implement `task`**

Render generation, PID/TGID, comm/state, mm summary fields, and special/unsupported
counts. If `get_task_mm()` fails, return `-ESRCH` without dereferencing a NULL mm.

**Step 4: Implement `vmas_ext`**

Render fixed key=value fields for normalized class/special/protection/dump policy,
backing identity, pgoff, raw flags, and path status. Continue after special VMAs;
do not treat unsupported metadata as a walker error.

**Step 5: Run the comparison gate**

Run: `./scripts/run-qemu.sh --ci --script tests/compare/diff-maps.sh`

Expected: `A1: PASS`, empty normalized diff, all class/magic assertions pass, and
clean dmesg.

**Step 6: Commit**

```bash
git add kernel_module tests/compare/diff-maps.sh
git commit -m "feat: expose A1 task and VMA views" -m "Assisted-by: Codex: GPT-5"
```

### Task 6: Add error-path and stress coverage

**Files:**
- Create: `tests/progs/many-vmas.c`
- Create: `tests/compare/a1-error-paths.sh`
- Modify: `tests/compare/diff-maps.sh`

**Step 1: Add stress fixture and failing assertions**

Create a static ARM64 fixture with at least 2000 non-mergeable VMAs by alternating
permissions. Add assertions for nonexistent PID, PID 2/kernel thread, target exit
during repeated reads, same-PID generation changes, non-admin rejection, and repeated
module load/unload.

Run: `sh -n tests/compare/a1-error-paths.sh`

Expected: syntax PASS; runtime failures identify unimplemented error paths.

**Step 2: Implement and verify error semantics**

Ensure all failed paths release task/mm references, unlock mmap locks, close seq
contexts, and leave the previous target intact. Use bounded loops for the exit race;
the test must run 100 iterations without oops or stale output.

Run: `./scripts/run-qemu.sh --ci --script tests/compare/a1-error-paths.sh`

Expected: `A1_ERRORS: PASS`, no kernel warning/oops/KASAN/lockdep diagnostics.

**Step 3: Commit**

```bash
git add tests/progs/many-vmas.c tests/compare/a1-error-paths.sh tests/compare/diff-maps.sh
git commit -m "test: cover A1 error and stress paths" -m "Assisted-by: Codex: GPT-5"
```

### Task 7: Integrate CI and static analysis

**Files:**
- Modify: `tests/ci-smoke.sh`
- Modify: `tests/module-smoke.sh`
- Modify: `.github/workflows/qemu-test.yml` (if required by current workflow)
- Modify: `docs/03-Iteration-Plan.md`
- Modify: `docs/steps/A1-readonly-probe.md`

**Step 1: Register the A1 gate**

Set the CI comparison list to include `tests/compare/diff-maps.sh`,
`tests/compare/a1-error-paths.sh`, and the target lifecycle gate. Keep the existing
dmesg marker check after each gate and preserve the exact final `CI_RESULT: PASS`
contract.

**Step 2: Run all guest gates**

Run: `./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh`

Expected: every A1 gate passes, dmesg remains clean after each gate, and the final
line is exactly `CI_RESULT: PASS`.

**Step 3: Run host-side static checks**

Run the repository's configured commands for:

```text
make -C kernel_module KDIR=/path/to/linux-5.10.29
scripts/checkpatch.pl --strict --no-tree kernel_module/core/*.c kernel_module/checkpoint/*.c
sparse ... kernel_module/...
git diff --check
```

Expected: module build succeeds, checkpatch has no errors, sparse has no
address-space warnings, and `git diff --check` is clean. Record unavailable tools
explicitly rather than silently skipping them.

**Step 4: Update completion criteria and commit**

Mark the A1 checklist complete only for checks actually run and passing. Update the
iteration plan to link both A1 plan documents.

```bash
git add tests/ci-smoke.sh tests/module-smoke.sh .github/workflows/qemu-test.yml docs/03-Iteration-Plan.md docs/steps/A1-readonly-probe.md
git commit -m "ci: gate A1 read-only probe" -m "Assisted-by: Codex: GPT-5"
```

## Final verification checklist

Before declaring A1 complete, run all of the following against a fresh 5.10.29
aarch64 QEMU boot:

1. `target-lifecycle.sh` passes, including failed target replacement preservation.
2. `vma-classifier-test.sh` reports all four classes and correct magic bytes.
3. `diff-maps.sh` has no normalized procfs diff.
4. `a1-error-paths.sh` passes 100 exit-race iterations and 2000+ VMA stress.
5. `tests/ci-smoke.sh` ends with exactly `CI_RESULT: PASS`.
6. dmesg has no BUG, WARNING, Oops, KASAN, lockdep, or suspicious RCU output.
7. module build, checkpatch, sparse, and `git diff --check` pass.
8. `criu_kernel.ko` loads/unloads repeatedly without residual debugfs state.

Only after this checklist passes should A1 be marked complete and A2 planning begin.
