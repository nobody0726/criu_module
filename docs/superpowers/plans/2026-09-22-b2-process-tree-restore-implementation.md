# B2 Process Tree Restore Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend B1 mini-restore from one single-threaded process to a multi-process tree with correct PID/ppid, session, pgid, synchronization, and cleanup semantics.

**Architecture:** Keep protobuf parsing, per-task image preparation, staging, and AArch64 `rt_sigreturn` in userspace. Add a tree coordinator that parses `pstree.img`, creates each child from the correct parent with `clone3(set_tid)`, restores sessions in two fork passes, restores pgid with timed futex waits, and reuses B1 for each task's independent address space. The kernel `VALIDATE -> COMMIT` ABI remains unchanged.

**Tech Stack:** C11 userspace, Linux 5.10.29/aarch64, `clone3(set_tid)`, `setsid`, `setpgid`, process-shared anonymous `mmap`, futex syscalls, CRIU image framing/protobuf wire helpers, shell contracts, static C fixtures, Lima `criu-dev` plus nested QEMU guest.

**Spec:** `docs/superpowers/specs/2026-09-22-b2-process-tree-restore-design.md`

## Global Constraints

- Target kernel: Linux 5.10.29.
- Target architecture: aarch64 only for live restore.
- Kernel does not parse protobuf, page images, paths, or register semantics.
- B2 core supports only single-threaded tasks with independent `mm_struct`.
- B2 core does not restore shared fd tables, pipe/socket, shmem, namespaces, cgroups, mount/fs context, or `CLONE_VM`.
- `clone3(set_tid)` is the only exact-PID creation path; no `ns_last_pid` fallback.
- Shared scratch is created before the first clone and is used only before each task's irreversible `COMMIT`.
- Every futex wait has a bounded timeout.
- Every created target PID is recorded immediately and cleaned individually on failure.
- Guest-local `/tmp` is the only authoritative QEMU restore workspace; do not use Lima 9p paths for mutable images or logs.
- Preserve B1 single-process behavior as a regression gate.
- Do not claim B2 PASS from a zero exit code; require PID topology, liveness, markers, and clean guest dmesg.

---

## File Map

Create:

- `userspace/mini-restore/rst_pstree.h` — topology node and parser interfaces.
- `userspace/mini-restore/rst_pstree.c` — `pstree.img` reader, topology validation, and node index.
- `userspace/mini-restore/rst_shared.h` — process-shared scratch and timed futex interfaces.
- `userspace/mini-restore/rst_shared.c` — scratch mapping, barriers, pgid state, and PID registry.
- `userspace/mini-restore/rst_fork.h` — recursive fork/session interfaces.
- `userspace/mini-restore/rst_fork.c` — two-pass recursive tree construction.
- `userspace/mini-restore/rst_session.h` — session and pgid restore interfaces.
- `userspace/mini-restore/rst_session.c` — `setsid`, `getsid`, `setpgid`, and timed leader waits.
- `userspace/mini-restore/rst_cleanup.h` — tree failure cleanup interfaces.
- `userspace/mini-restore/rst_cleanup.c` — individual PID termination and reaping.
- `userspace/mini-restore/task_restore.h` — reusable per-task B1 restore interface.
- `userspace/mini-restore/task_restore.c` — extracted B1 image/staging/VALIDATE/COMMIT preparation.
- `tests/progs/tree-session.c` — static fixture with mixed session/pgid topology and markers.
- `tests/progs/tree-invalid-leader.c` — unsupported topology fixture.
- `tests/b2-pstree-contract.sh` — parser/topology contract tests.
- `tests/b2-shared-contract.sh` — scratch/futex/PID registry contract tests.
- `tests/b2-negative.sh` — PID conflict, timeout, unsupported topology, and cleanup tests.
- `tests/b2-restore.sh` — Linux 5.10.29/aarch64 QEMU authoritative gate.

Modify:

- `userspace/mini-restore/Makefile` — compile the B2 modules.
- `userspace/mini-restore/restore.h` — add shared status and per-task topology references only where needed.
- `userspace/mini-restore/main.c` — preserve B1 mode and add `--pstree`/`--restore-sibling` dispatch.
- `userspace/mini-restore/carrier.c` — expose the existing exact-PID carrier wrapper to tree code without changing rseq cleanup semantics.
- `tests/progs/Makefile` — build B2 fixtures.
- `docs/steps/B2-pstree-restore.md` — update implementation status and gate evidence.
- `docs/03-Iteration-Plan.md` — mark only the B2 core status after the authoritative gate passes.

Do not modify:

- `include/criu_restore_abi.h` unless a separately reviewed, versioned ABI extension becomes unavoidable.
- Existing B1 kernel patches.

---

### Task 1: Add Topology Model and Real `pstree.img` Parser

**Files:**
- Create: `userspace/mini-restore/rst_pstree.h`
- Create: `userspace/mini-restore/rst_pstree.c`
- Modify: `userspace/mini-restore/Makefile`
- Create: `tests/b2-pstree-contract.sh`
- Create: `tests/progs/tree-session.c`
- Modify: `tests/progs/Makefile`

**Interfaces:**

```c
struct rst_item {
	pid_t pid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	unsigned flags;
	unsigned index;
	struct rst_item *parent;
	struct rst_item *children;
	struct rst_item *next_sibling;
	struct b1_restore_image *image;
};

struct rst_pstree {
	struct rst_item *items;
	size_t count;
	struct rst_item *root;
};

enum b1_restore_status rst_read_pstree(const char *dir,
				       struct rst_pstree *tree);
void rst_free_pstree(struct rst_pstree *tree);
enum b1_restore_status rst_validate_pstree(const struct rst_pstree *tree,
					    struct b1_restore_image *diag);
```

- [ ] **Step 1: Write the failing parser contract.** Build a minimal CRIU-framed `pstree.img` fixture containing only the real `pstree.proto` fields (`pid`, `ppid`, `pgid`, `sid`, and one `threads` entry) for a root, two children, and one grandchild with distinct `sid`/`pgid` values. Assert the parser derives the exact `born_sid`, parent indexes, and root; the fixture must not add a non-existent `born_sid` protobuf field.
- [ ] **Step 2: Run the contract to verify it fails.**

Run:

```sh
sh tests/b2-pstree-contract.sh
```

Expected: FAIL because `rst_read_pstree()` and the B2 object files do not exist.

- [ ] **Step 3: Implement framing and protobuf field extraction.** Reuse the existing CRIU wire/framing helpers in `criu_image_reader.c`; parse only the real `pstree_entry` fields `pid`, `ppid`, `pgid`, `sid`, and repeated `threads`. Do not add a second protobuf decoder or expect a `born_sid` field.
- [ ] **Step 4: Implement topology validation and `born_sid` derivation.** Reject duplicate PIDs, missing non-root parents, cycles, multiple roots, `threads` arrays with anything other than one leader, shared-mm markers, unsupported namespace markers, missing required session/pgid leaders, and conflicting ancestor-derived `born_sid` values. Use the CRIU `prepare_pstree_ids()` rule: for each non-leader task whose parent has a different session, walk ancestors until the target session leader and mark each traversed ancestor with that session.
- [ ] **Step 5: Add the static `tree-session` fixture.** Emit stable markers containing name, PID, PPID, PGID, SID, and a tick counter. Keep the fixture single-threaded and use only private anonymous mappings.
- [ ] **Step 6: Run the contract to verify it passes.**

Run:

```sh
make -C userspace/mini-restore clean all
make -C tests/progs tree-session
sh tests/b2-pstree-contract.sh
```

Expected: `B2_PSTREE_CONTRACT: PASS`.

- [ ] **Step 7: Commit.**

```sh
git add userspace/mini-restore/rst_pstree.* userspace/mini-restore/Makefile \
  tests/b2-pstree-contract.sh tests/progs/tree-session.c tests/progs/Makefile
git commit -m "feat: parse and validate B2 process trees"
```

### Task 2: Build Shared Scratch, Timed Futexes, and PID Registry

**Files:**
- Create: `userspace/mini-restore/rst_shared.h`
- Create: `userspace/mini-restore/rst_shared.c`
- Create: `tests/b2-shared-contract.sh`
- Modify: `userspace/mini-restore/Makefile`

**Interfaces:**

```c
struct rst_shared;

enum b1_restore_status rst_shared_init(size_t task_count,
				       struct rst_shared **out);
void rst_shared_destroy(struct rst_shared *shared);
int rst_shared_register_pid(struct rst_shared *shared, size_t index, pid_t pid);
int rst_shared_mark(struct rst_shared *shared, size_t index, unsigned state);
int rst_shared_wait_count(struct rst_shared *shared, unsigned expected,
			  unsigned timeout_ms);
int rst_shared_wait_flag(struct rst_shared *shared, unsigned *word,
			 unsigned expected, unsigned timeout_ms);
int rst_shared_abort(struct rst_shared *shared, int error_code);
int rst_shared_is_aborted(const struct rst_shared *shared);
```

- [ ] **Step 1: Write the failing shared-state contract.** Verify the mapping is `MAP_SHARED`, two forked processes observe the same counter, timed waits return on success, and timed waits return failure when no participant arrives.
- [ ] **Step 2: Run it and confirm failure.**

Run:

```sh
sh tests/b2-shared-contract.sh
```

Expected: FAIL because the shared-state module is absent.

- [ ] **Step 3: Implement the fixed-layout scratch allocation.** Allocate one mapping containing header, per-task states, per-pgid futex words, and PID registry. Check multiplication and page-size overflow before `mmap`.
- [ ] **Step 4: Implement futex wait/wake with monotonic deadlines.** Use `FUTEX_WAIT`/`FUTEX_WAKE` on shared words, retry `EINTR`, and return a diagnostic timeout instead of looping forever.
- [ ] **Step 5: Implement abort propagation.** Set an atomic abort flag and wake every waiter when any task fails.
- [ ] **Step 6: Run the shared contract to verify it passes.**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b2-shared-contract.sh
```

Expected: `B2_SHARED_CONTRACT: PASS`.

- [ ] **Step 7: Commit.**

```sh
git add userspace/mini-restore/rst_shared.* userspace/mini-restore/Makefile \
  tests/b2-shared-contract.sh
git commit -m "feat: add B2 shared restore coordination"
```

### Task 3: Extract Reusable Per-Task B1 Restore

**Files:**
- Create: `userspace/mini-restore/task_restore.h`
- Create: `userspace/mini-restore/task_restore.c`
- Modify: `userspace/mini-restore/main.c`
- Modify: `userspace/mini-restore/Makefile`
- Modify: `userspace/mini-restore/carrier.c`
- Modify: `userspace/mini-restore/carrier.h`
- Add regression assertions to: `tests/b1-kernel-assisted-restore.sh`, `tests/b1-negative.sh`

**Interfaces:**

```c
struct b2_task_restore {
	struct b1_restore_image image;
	struct b1_staging_plan staging;
	struct b1_cleanup cleanup;
	struct b1_aarch64_rt_sigframe sigframe;
	struct b1_bootstrap_args bootstrap;
	int restore_fd;
	pid_t target_pid;
	unsigned prepared;
};

enum b1_restore_status b1_task_restore_prepare(
	const char *images, pid_t target_pid, struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_validate(
	struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_commit(
	struct b2_task_restore *task);
void b1_task_restore_destroy(struct b2_task_restore *task);
```

- [ ] **Step 1: Add a B1 single-process regression test before refactoring.** Run the current B1 dry-run and live path and capture the expected markers.
- [ ] **Step 2: Extract image read, validation, staging, sigframe, bootstrap mapping, and `VALIDATE` preparation from `main.c` into `task_restore.c` without changing behavior.**
- [ ] **Step 3: Keep rseq unregister ordering in the carrier path.** The extracted task API must not move or duplicate the child-side rseq cleanup that fixed the B1 liveness failure.
- [ ] **Step 4: Make the existing B1 CLI call the extracted API.** `--images DIR` remains unchanged and continues to represent a one-node tree.
- [ ] **Step 5: Run the B1 local contracts and build.**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b1-carrier-contract.sh
sh tests/b1-negative.sh
```

Expected: all existing B1 contracts remain PASS.

- [ ] **Step 6: Commit.**

```sh
git add userspace/mini-restore/task_restore.* userspace/mini-restore/main.c \
  userspace/mini-restore/carrier.* userspace/mini-restore/Makefile \
  tests/b1-kernel-assisted-restore.sh tests/b1-negative.sh
git commit -m "refactor: expose reusable B1 task restore"
```

### Task 4: Implement Recursive Fork and Two-Pass Session Construction

**Files:**
- Create: `userspace/mini-restore/rst_fork.h`
- Create: `userspace/mini-restore/rst_fork.c`
- Create: `userspace/mini-restore/rst_session.h`
- Create: `userspace/mini-restore/rst_session.c`
- Modify: `userspace/mini-restore/carrier.h`
- Modify: `userspace/mini-restore/Makefile`

**Interfaces:**

```c
int rst_restore_sid(const struct rst_item *item);
int rst_before_setsid(const struct rst_item *child);
int rst_create_children_and_session(struct rst_item *item,
				    struct rst_shared *shared,
				    struct b2_task_restore *task);
```

- [ ] **Step 1: Write a topology-only fork test.** Use a synthetic tree model and a test callback instead of B1 commit; assert every child is created by the expected parent and that the two session passes produce the expected `getsid()` values.
- [ ] **Step 2: Run the test to confirm the recursive coordinator is absent.**
- [ ] **Step 3: Implement the first pass.** Iterate only children satisfying `born_sid != -1` or a validated session mismatch; call the existing exact-PID carrier wrapper; register each PID immediately.
- [ ] **Step 4: Implement `rst_restore_sid()`.** Only a task whose target PID equals target SID calls `setsid()`. Other tasks call `getsid(0)` and compare; return `-EOPNOTSUPP` for unsupported session topology.
- [ ] **Step 5: Implement the second pass.** Fork remaining children; the child branch immediately recurses and never returns to the parent's sibling loop.
- [ ] **Step 6: Add `--restore-sibling` parsing.** Use the existing carrier flags to select `CLONE_PARENT` for the root only, and document that external original `ppid` is not exactly restorable.
- [ ] **Step 7: Run the topology contract.**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b2-pstree-contract.sh
```

Expected: `B2_PSTREE_CONSTRUCTION: PASS`.

- [ ] **Step 8: Commit.**

```sh
git add userspace/mini-restore/rst_fork.* userspace/mini-restore/rst_session.* \
  userspace/mini-restore/carrier.* userspace/mini-restore/Makefile \
  tests/b2-pstree-contract.sh
git commit -m "feat: construct process trees with session ordering"
```

### Task 5: Restore pgid, Ready Barrier, and Commit Release

**Files:**
- Modify: `userspace/mini-restore/rst_session.c`
- Modify: `userspace/mini-restore/rst_shared.c`
- Create: `tests/progs/tree-pgid.c`
- Modify: `tests/progs/Makefile`
- Modify: `tests/b2-shared-contract.sh`

**Interfaces:**

```c
int rst_restore_pgid(struct rst_item *item, struct rst_shared *shared,
		     unsigned timeout_ms);
int rst_mark_ready(struct rst_shared *shared, size_t index);
int rst_wait_all_ready(struct rst_shared *shared, unsigned timeout_ms);
int rst_wait_commit_release(struct rst_shared *shared, unsigned timeout_ms);
```

- [ ] **Step 1: Add a failing pgid fixture.** Create a tree where a process joins a group whose leader is created in another branch, and assert the member cannot finish until the leader marks `pgrp_set`.
- [ ] **Step 2: Implement leader-index lookup and timed wait.** Never wait on a raw PID without resolving it to the shared registry index.
- [ ] **Step 3: Implement `setpgid(0, target_pgid)` and postcondition `getpgid(0)`.** Mark the leader only after the postcondition passes.
- [ ] **Step 4: Implement the ready barrier and commit release.** The barrier must happen before any B1 `COMMIT`; no scratch-dependent barrier may occur after address-space replacement.
- [ ] **Step 5: Exercise timeout and abort wakeups.**

Run:

```sh
make -C tests/progs tree-pgid
sh tests/b2-shared-contract.sh
```

Expected: `B2_PGID_BARRIER: PASS` and timeout coverage.

- [ ] **Step 6: Commit.**

```sh
git add userspace/mini-restore/rst_session.c userspace/mini-restore/rst_shared.c \
  tests/progs/tree-pgid.c tests/progs/Makefile tests/b2-shared-contract.sh
git commit -m "feat: restore pgid with timed barriers"
```

### Task 6: Add Tree Failure Cleanup and Main Orchestration

**Files:**
- Create: `userspace/mini-restore/rst_cleanup.h`
- Create: `userspace/mini-restore/rst_cleanup.c`
- Modify: `userspace/mini-restore/main.c`
- Modify: `userspace/mini-restore/restore.h`
- Modify: `userspace/mini-restore/Makefile`
- Create: `tests/b2-negative.sh`

**Interfaces:**

```c
int rst_cleanup_all(struct rst_shared *shared,
		    const struct rst_pstree *tree,
		    unsigned timeout_ms);
int rst_wait_tree_root(pid_t root_pid, unsigned timeout_ms);
```

- [ ] **Step 1: Write negative tests for PID conflict, missing leader, barrier timeout, and one task failing before ready.** The assertions must check a nonzero diagnostic, no permanent hang, and disappearance of every recorded target PID.
- [ ] **Step 2: Implement a PID-by-PID cleanup pass.** Send `SIGKILL` to every live registered PID, retry `ESRCH` as success, wait direct children where possible, and poll `/proc/$pid` for descendants.
- [ ] **Step 3: Implement parent orchestration.** Add `--pstree DIR` and `--restore-sibling`; preserve `--images DIR` as the B1 single-process path.
- [ ] **Step 4: Wire the phases in this order:**

```text
read/validate all nodes
-> map shared scratch
-> create root
-> recursively fork/session
-> restore pgid
-> prepare each B1 task and VALIDATE
-> ready barrier
-> release COMMIT
-> wait root or detached completion
```

- [ ] **Step 5: On any pre-commit error, set shared abort before cleanup.** Wake every futex waiter before killing registered tasks so no child remains blocked.
- [ ] **Step 6: On post-commit abnormal exit, treat restore as failed and clean all registered PIDs individually.** Do not claim rollback.
- [ ] **Step 7: Run negative tests and B1 regression.**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b2-negative.sh
sh tests/b1-negative.sh
```

Expected: `B2_NEGATIVE: PASS` and `B1_NEGATIVE: PASS`.

- [ ] **Step 8: Commit.**

```sh
git add userspace/mini-restore/rst_cleanup.* userspace/mini-restore/main.c \
  userspace/mini-restore/restore.h userspace/mini-restore/Makefile \
  tests/b2-negative.sh
git commit -m "feat: add B2 tree orchestration and cleanup"
```

### Task 7: Add Real-Image Tree Gate and Guest Fixture

**Files:**
- Modify: `tests/progs/tree-session.c`
- Modify: `tests/progs/tree-pgid.c`
- Create: `tests/progs/tree-invalid-leader.c`
- Modify: `tests/progs/Makefile`
- Create: `tests/b2-restore.sh`

**Interfaces:**

```text
B2_PROCESS_TREE_RESTORE: PASS
B2_PROCESS_TREE_RESTORE: SKIP <reason>
B2_PROCESS_TREE_RESTORE: FAIL <reason>
```

- [ ] **Step 1: Write the guest gate around a real CRIU dump.** Start `tree-session`, record each task's marker/PID/PPID/PGID/SID, dump with real CRIU, kill the original tree, and invoke `mini-restore --pstree`.
- [ ] **Step 2: Verify the gate fails before the full tree path is wired.**
- [ ] **Step 3: Check restored liveness and topology.** For every expected PID, read `/proc/$pid/stat` fields 4-6 (PPID, PGID, SID) and the fixture marker; compare them with the pre-dump records. Do not depend on a non-standard `/proc/$pid/sessionid` file.
- [ ] **Step 4: Check all restored tasks' B1 state.** Validate private mapping presence, marker bytes, tick progression, and TLS-sensitive execution.
- [ ] **Step 5: Check dmesg from a marked baseline.** Reject Oops, BUG, WARNING, KASAN, refcount, use-after-free, or `criu_restore` errors.
- [ ] **Step 6: Keep all mutable paths below guest-local `/tmp`.** The outer command must enter Lima first and start QEMU through `scripts/run-qemu.sh`.
- [ ] **Step 7: Run the gate in the authoritative environment.**

Run:

```sh
limactl shell criu-dev bash -lc \
  'set -o pipefail; \
   export CRIU_SOURCE=/home/yhome.guest/kernels/verify-a3-task5/criu; \
   cd /Users/yhome/workspace/source_code/criu_module; \
   ./scripts/run-qemu.sh --ci --script tests/b2-restore.sh'
```

Expected: `B2_PROCESS_TREE_RESTORE: PASS`, with no dmesg fault markers.

- [ ] **Step 8: Commit.**

```sh
git add tests/progs/tree-session.c tests/progs/tree-pgid.c \
  tests/progs/tree-invalid-leader.c tests/progs/Makefile tests/b2-restore.sh \
  scripts/run-qemu.sh
git commit -m "test: add B2 process tree restore gate"
```

### Task 8: Complete B2 Matrix and Documentation

**Files:**
- Modify: `tests/b2-restore.sh`
- Modify: `tests/b2-negative.sh`
- Modify: `docs/steps/B2-pstree-restore.md`
- Modify: `docs/03-Iteration-Plan.md`
- Create: `docs/plans/2026-09-22-b2-verification.md`

- [ ] **Step 1: Add the required matrix cases.** Cover flat tree, five-level chain, mixed sessions, multiple pgids, root sibling mode, PID conflict, unsupported dead leader, barrier timeout, 50-task tree, and one-process B1 regression.
- [ ] **Step 2: Keep pipe/socket connectivity out of B2.** Record it as B2-E4 instead of adding a false-positive test.
- [ ] **Step 3: Record exact command, guest kernel, commit, marker, topology comparison, and dmesg result in the verification document.**
- [ ] **Step 4: Update `docs/steps/B2-pstree-restore.md` only with evidence-backed status.** Do not mark unsupported backlog entries as complete.
- [ ] **Step 5: Mark B2 core complete in `docs/03-Iteration-Plan.md` only after the fresh guest gate and full matrix pass.**
- [ ] **Step 6: Run final local checks.**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b2-pstree-contract.sh
sh tests/b2-shared-contract.sh
sh tests/b2-negative.sh
sh tests/b1-negative.sh
git diff --check
```

Expected: all local contracts PASS and no whitespace errors.

- [ ] **Step 7: Commit the evidence and documentation.**

```sh
git add tests/b2-restore.sh tests/b2-negative.sh \
  docs/steps/B2-pstree-restore.md docs/03-Iteration-Plan.md \
  docs/plans/2026-09-22-b2-verification.md
git commit -m "docs: record B2 process tree restore verification"
```

### Task 9: Final Integration Verification

**Files:**
- No new source files.
- Review all B2 changes and the B1 regression surface.

- [ ] **Step 1: Verify the working tree and commit range.**

Run:

```sh
git status --short --branch
git log --oneline --decorate -12
git diff --check
git show --check --format=oneline HEAD
```

Expected: only intentional B2 commits and the pre-existing untracked `artifacts/` directory.

- [ ] **Step 2: Re-run the full B1 local contract set.**
- [ ] **Step 3: Re-run the full B2 local contract set.**
- [ ] **Step 4: Re-run the Linux 5.10.29/aarch64 QEMU B2 gate.**
- [ ] **Step 5: Confirm the final marker is exactly `B2_PROCESS_TREE_RESTORE: PASS`.**
- [ ] **Step 6: Confirm all unsupported B-track items remain listed in the plan backlog.**
- [ ] **Step 7: Commit only if the evidence document changed after the final run.**

---

## B 轨后续待办（B2 核心完成后执行）

这些项目保留在 B2 计划末尾，作为明确 backlog；它们不属于上述 B2 核心任务，也不
能由 `B2_PROCESS_TREE_RESTORE: PASS` 推断为已完成。

| 编号 | 后续任务 | 前置/依赖 | 状态 |
|---|---|---|---|
| B2-E1 | TASK_HELPER、session leader 已退出拓扑 | B2 核心树模型 | 待设计 |
| B2-E2 | 根任务外部 ppid 的更完整 sibling/parent 语义 | 调用者模型、pid namespace | 已知限制 |
| B2-E3 | 多线程、`CLONE_THREAD`、`CLONE_VM` | A4、B2 核心屏障 | 待设计 |
| B2-E4 | 共享 fd table、pipe、UNIX/TCP socket | A5/A8、soccr 语义 | 待设计 |
| B2-E5 | shmem、memfd、POSIX/SysV shm | A8、namespace 语义 | 待设计 |
| B2-E6 | PID/mount/network/user namespace | A9、PID namespace | 待设计 |
| B2-E7 | cgroup、mount、cwd/root 和 fs context | A9、权限顺序 | 待设计 |
| B2-E8 | dirty file-private/COW、vDSO relocation | B1 VMA 扩展 | 待设计 |
| B2-E9 | timer、credentials、seccomp、LSM | A6、A9 | 待设计 |
| B2-E10 | target PID occupied、duplicate COMMIT live-kernel 测试 | Linux guest 扩展矩阵 | 待验证 |
| B2-E11 | 完整 ZDTM restore allowlist 和跨场景矩阵 | B2 核心及全部资源扩展 | 待规划 |

Backlog 执行规则：

1. 每个条目单独写设计和验收标准；
2. 先确认它属于 B2 扩展还是新的 B3；
3. 不能通过放宽 validator、静态 allowlist 或跳过 guest gate 来宣称完成；
4. 每个完成项都要补充真实 CRIU 镜像、Linux 5.10.29 guest 和失败路径证据。
