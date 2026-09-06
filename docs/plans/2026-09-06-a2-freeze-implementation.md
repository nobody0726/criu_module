# A2 Freeze/Thaw Spike Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement and verify a synchronous cgroup-v2 freezer primitive for the A1-selected thread group, including settled detection, original-state restoration, and complete rollback.

**Architecture:** Extend A1's mutex-protected target state with a single freeze context owned by `checkpoint/freeze.c`. The context pins the target thread group, records original cgroup and stopped-state information, moves tasks into a temporary cgroup-v2 freezer, and waits until every task is off-CPU. `core/main.c` exposes `freeze`, `thaw`, and an expanded snapshot-style `status`; descendants remain out of scope until a separate `freeze_tree` phase.

**Tech Stack:** Linux 5.10.29 aarch64 kernel module, cgroup v2 freezer, debugfs/seq_file, C, QEMU guest tests, shell fixtures, sparse, checkpatch.

---

## Working rules

- Run all module load, cgroup, and task-state tests inside `./scripts/run-qemu.sh`; never `insmod` on macOS or Lima directly.
- Preserve the untracked `artifacts/` directory.
- Treat the maintained kernel-wrapper patch and its exported GPL ABI as a hard gate. If the patch does not apply or the target kernel cannot export the wrapper to a GPL out-of-tree module, stop after Task 1 and report `A2_FREEZER: BLOCKED`; do not substitute per-task APIs or a user-space helper.
- Use TDD for each behavior: add or extend the guest test, run it to capture the expected failure, implement the smallest change, rerun the focused gate, then commit.
- Keep A1 read views generation-safe while a freeze context is active. Do not add dump/image code in A2.

### Task 1: Add the 5.10.29 wrapper patch and establish the feasibility gate

**Files:**
- Create: `patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch`
- Create: `scripts/apply-kernel-patches.sh`
- Create: `spike/probes/a2-freezer-symbols.c`
- Create: `spike/probes/Makefile`
- Create: `tests/compare/freezer-symbols.sh`
- Modify: `scripts/build-kernel.sh` to apply the patch after unpacking and before `olddefconfig`

**Step 1: Write the compile probe**

Add a minimal high-level GPL wrapper inside the kernel cgroup core. It must create a uniquely named child cgroup, attach the complete target thread group, freeze it, retain the original cgroup reference/path in an opaque cookie, and reverse the operation on thaw. Export only the wrapper ABI; keep all calls to `cgroup_create()`, `cgroup_attach_task()`, and `cgroup_freeze()` inside the patched kernel source. Update the isolated probe to include the wrapper header and call only the exported wrapper symbols without loading the probe.

**Step 2: Run the probe and guest preflight**

Run:

```bash
scripts/apply-kernel-patches.sh $HOME/kernels/linux-5.10.29
make -C spike/probes KDIR=$HOME/kernels/linux-5.10.29
./scripts/run-qemu.sh --ci --script tests/compare/freezer-symbols.sh
```

Expected: the patch applies cleanly, the probe either exits 0 and prints the wrapper symbol list, or exits nonzero with `A2_FREEZER: BLOCKED` and the unresolved wrapper symbols. The guest script must verify a mounted cgroup2 hierarchy and functional child `cgroup.freeze`/`cgroup.events` files; it must not look for `freezer` in `cgroup.controllers`.

**Step 3: Commit the gate**

```bash
git add patches scripts spike/probes tests/compare/freezer-symbols.sh
git commit -m "test: gate A2 on cgroup freezer symbols" -m "Assisted-by: Codex: GPT-5"
```

### Task 2: Add freeze context types and target-state coordination

**Files:**
- Create: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Modify: `kernel_module/core/target.c`
- Test: `tests/compare/freeze-errors.sh`

**Step 1: Extend the failing error test**

Add checks that `freeze` without a target returns `ESRCH`, a second freeze returns `EBUSY`, and writing `target` while a context exists returns `EBUSY` while preserving the original generation.

Run:

```bash
sh -n tests/compare/freeze-errors.sh
```

Expected: syntax passes; the runtime checks fail because the files and context do not exist yet.

**Step 2: Define the context and state machine**

Add the opaque `struct criu_freeze_ctx`, the `criu_freeze`, `criu_thaw`, and `criu_freeze_settled` declarations, and a mutex-protected module-global context with the states `IDLE`, `FREEZING`, `FROZEN_SETTLED`, `THAWING`, and `ROLLBACK`. Add a target-lock query used by `criu_target_set()` so target replacement is rejected with `-EBUSY` whenever a context is present.

**Step 3: Implement the minimal context ownership path**

Pin the A1 target reference and generation before publishing `FREEZING`; release them on every error path. Reject malformed or duplicate requests without mutating the existing context.

**Step 4: Run the focused gate**

Run the guest error script only after the real debugfs files exist. Before that point, use the module build as the compile gate; do not add test-only debugfs stubs that could hide interface errors.

**Step 5: Commit**

```bash
git add kernel_module/include/criu_kernel.h kernel_module/checkpoint/freeze.c kernel_module/core/target.c tests/compare/freeze-errors.sh
git commit -m "feat: add A2 freeze context state" -m "Assisted-by: Codex: GPT-5"
```

### Task 3: Implement thread-group enumeration and original-state capture

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Create: `tests/progs/multithread-counter.c`
- Create: `tests/progs/stopped-counter.c`
- Modify: `tests/progs/Makefile`
- Create: `tests/compare/freeze-stopped.sh`

**Step 1: Add fixture contracts**

Build ARM64 static fixtures. `multithread-counter` must create several threads with independent heartbeat counters in shared memory. `stopped-counter` must expose a deterministic way for the test harness to capture a thread group's pre-freeze stopped state.

Run the documented cross-build command from `tests/progs/Makefile`; verify each output with `file` and run it only in the QEMU guest.

**Step 2: Write the failing assertions**

Extend `tests/compare/freeze-test.sh` to require the expected thread count and to compare every heartbeat before, during, and after freeze. Add a stopped-state assertion to `freeze-errors.sh` or a focused `freeze-stopped.sh`.

**Step 3: Implement safe enumeration**

Under the target task's signal/RCU-safe iteration rules for Linux 5.10.29, pin every member of the thread group, store its tid and original stopped flag, and abort with one cleanup path if a member exits. Do not enumerate descendants in this task. Define `criu_freeze_settled()` to return false for any pinned task that is running or runnable.

**Step 4: Run focused tests**

Run:

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-test.sh
./scripts/run-qemu.sh --ci --script tests/compare/freeze-stopped.sh
```

Expected: fixture startup and pre-freeze state capture are validated; cgroup movement remains the next task's failure.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/freeze.c kernel_module/include/criu_kernel.h tests/progs
git commit -m "feat: capture A2 thread group state" -m "Assisted-by: Codex: GPT-5"
```

### Task 4: Add the cgroup-v2 freezer adapter and synchronous settle loop

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/Makefile`
- Test: `tests/compare/freeze-test.sh`

**Step 1: Define the expected success path**

Make `freeze-test.sh` start `busy-counter`, record `/proc/PID/cgroup`, set `target`, write `1` to `freeze`, and require the write to block until `status` reports `freeze_state=frozen` and `freeze_settled=1`.

Run the script before implementation. Expected: failure at the missing cgroup adapter.

**Step 2: Implement cgroup ownership and movement**

Using only the exported wrapper ABI accepted by Task 1, record the complete original cgroup-v2 path, create a uniquely named temporary freezer cgroup, move all pinned thread-group tasks into it, and request frozen state. Keep the opaque cookie until thaw or rollback. Return `-EOPNOTSUPP` if the patched adapter preconditions are not met.

**Step 3: Implement synchronous settle**

Poll `criu_freeze_settled()` every 10 ms until all tasks are no longer running/runnable. Use module parameter `settle_timeout_ms` with default 5000. On expiry return `-ETIMEDOUT` through the common rollback path. Never sleep while holding a spinlock or an mmap/cgroup lock that the freezer path may reacquire.

**Step 4: Verify success**

Run:

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-test.sh
```

Expected: `A2_FREEZE: PASS`; the counter stops during the synchronous write, every thread is settled, and the counter resumes after thaw.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/freeze.c kernel_module/Makefile tests/compare/freeze-test.sh
git commit -m "feat: freeze thread groups with cgroup v2" -m "Assisted-by: Codex: GPT-5"
```

### Task 5: Implement thaw, cgroup restoration, and stopped-state restoration

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/core/main.c`
- Test: `tests/compare/freeze-test.sh`
- Create: `tests/compare/freeze-stopped.sh`

**Step 1: Add failing thaw checks**

Require `thaw` to restore the exact `/proc/PID/cgroup` contents, resume a running counter, and leave an initially stopped group stopped. Require `thaw` without a context to return `ENOENT`.

**Step 2: Implement reverse ordering**

Add the `thaw` operation and reverse the context in this order: leave the temporary frozen state, restore original cgroup membership, restore recorded stopped states, release temporary cgroup resources, then drop task references and publish `IDLE`. Use the context's pinned task objects, never the current numeric PID.

**Step 3: Verify**

Run:

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-test.sh
./scripts/run-qemu.sh --ci --script tests/compare/freeze-stopped.sh
```

Expected: both pass and dmesg remains free of lockdep, atomic-sleep, and freezer warnings.

**Step 4: Commit**

```bash
git add kernel_module/checkpoint/freeze.c kernel_module/core/main.c tests/compare/freeze-test.sh tests/compare/freeze-stopped.sh
git commit -m "feat: restore A2 cgroup and stopped state" -m "Assisted-by: Codex: GPT-5"
```

### Task 6: Add status snapshots, permissions, and unload safety

**Files:**
- Modify: `kernel_module/core/main.c`
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/include/criu_kernel.h`
- Test: `tests/compare/freeze-errors.sh`

**Step 1: Add status/error assertions**

Check all approved status fields, `CAP_SYS_ADMIN` failures, malformed writes, duplicate freeze, target replacement, and target exit. Ensure a failed target lookup leaves the previous target unchanged.

**Step 2: Render snapshot-style status**

Expand `status` with the approved freeze fields. Copy values under the freeze mutex into a local structure before calling `seq_printf`, so one read cannot mix generations or contexts.

**Step 3: Make unload safe**

On module exit, synchronously run the same thaw/rollback cleanup before removing debugfs. Never leave a target task in the temporary freezer cgroup when `rmmod` returns. Assert that an active freeze is gone and the original cgroup/state are restored after unload and reload.

**Step 4: Verify and commit**

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-errors.sh
git add kernel_module/core/main.c kernel_module/checkpoint/freeze.c kernel_module/include/criu_kernel.h tests/compare/freeze-errors.sh
git commit -m "feat: expose A2 freeze status and guards" -m "Assisted-by: Codex: GPT-5"
```

### Task 7: Exercise deterministic rollback and task-exit races

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Create: `tests/compare/freeze-rollback.sh`
- Modify: `tests/compare/freeze-errors.sh`

**Step 1: Write failure-first tests**

Set `settle_timeout_ms=0` and require `ETIMEDOUT`, `freeze_state=idle`, unchanged `/proc/PID/cgroup`, no pinned-context residue, and a subsequent successful target selection. Add a target-exit race and verify no oops or stale debugfs state.

Run:

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-rollback.sh
```

Expected: failure until the common rollback path is complete.

**Step 2: Harden the single cleanup path**

Make rollback thaw tasks already moved, restore cgroup membership, restore original stopped states, destroy temporary cgroup resources, release every task reference, and publish `last_error` only after cleanup. Preserve the original errno for the caller.

**Step 3: Verify and commit**

```bash
./scripts/run-qemu.sh --ci --script tests/compare/freeze-rollback.sh
./scripts/run-qemu.sh --ci --script tests/compare/freeze-errors.sh
git add kernel_module/checkpoint/freeze.c tests/compare/freeze-rollback.sh tests/compare/freeze-errors.sh
git commit -m "test: cover A2 freeze rollback paths" -m "Assisted-by: Codex: GPT-5"
```

### Task 8: Integrate the A2 gates and update project documentation

**Files:**
- Modify: `tests/ci-smoke.sh`
- Modify: `docs/steps/A2-freeze.md`
- Modify: `docs/03-Iteration-Plan.md` if the phase status table needs an A2 plan link
- Test: `.github/workflows/qemu-test.yml` only if the existing workflow omits the smoke gate

**Step 1: Add CI gates**

Append `freezer-symbols.sh`, `freeze-test.sh`, `freeze-stopped.sh`, `freeze-errors.sh`, and `freeze-rollback.sh` to the existing guest gate list. Missing scripts must remain hard failures, never skips.

**Step 2: Reconcile documentation**

Link the approved design and this implementation plan from `docs/steps/A2-freeze.md`. Mark descendants/tree freezing as the next stage and remove any stale statement that contradicts the synchronous target-group contract.

**Step 3: Run the complete validation set**

```bash
make -C kernel_module KDIR=$HOME/kernels/linux-5.10.29
make -C kernel_module KDIR=$HOME/kernels/linux-5.10.29 C=1 CF=-D__CHECK_ENDIAN__
./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh
git diff --check
```

Expected: build and sparse pass, every A2 gate emits its PASS marker, dmesg remains clean, and the final smoke line is `CI_RESULT: PASS`.

**Step 4: Commit the integration**

```bash
git add tests/ci-smoke.sh docs/steps/A2-freeze.md docs/03-Iteration-Plan.md .github/workflows/qemu-test.yml
git commit -m "test: integrate A2 freeze gates" -m "Assisted-by: Codex: GPT-5"
```

## Completion checklist

- [ ] Task 1 applies the 5.10.29 wrapper patch and proves its exported GPL ABI or records a hard block.
- [ ] Thread-group freeze is synchronous and settled before success.
- [ ] Original cgroup membership and stopped state are restored by thaw.
- [ ] Duplicate operations, permissions, exits, timeout, and unload behavior are tested.
- [ ] No descendants/tree controls are exposed in the first-stage module.
- [ ] Kernel build, sparse, QEMU smoke, and dmesg hygiene all pass.
