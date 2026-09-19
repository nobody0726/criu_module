# B1 Kernel-Assisted Restore Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Restore a real CRIU single-process image on the Linux 5.10.29/aarch64 guest by parsing and staging in userspace, committing the final VMA layout through a kernel transaction, and finishing with userspace aarch64 `rt_sigreturn`.

**Architecture:** Userspace owns CRIU protobuf parsing, validation, backing-file reopening, carrier creation, staging mappings, page population, sigframe construction, and cleanup. A Linux 5.10.29 kernel patch adds `/dev/criu_restore` with `VALIDATE` and one-way `COMMIT`; the kernel copies the complete plan before commit and performs the final current-task VMA transaction. A position-independent bootstrap then sets `TPIDR_EL0` and invokes the standard kernel `rt_sigreturn` path.

**Tech Stack:** C11 userspace, protobuf-c/CRIU image definitions, Linux 5.10.29 kernel patch, aarch64 assembly, QEMU guest launched through Lima `criu-dev`, shell/Python contract tests, real CRIU dump as the input oracle.

**Spec:** `docs/superpowers/specs/2026-09-19-b1-kernel-assisted-restore-design.md`

**Required references before implementation:** `criu/criu/cr-restore.c`, `criu/criu/mem.c`, `criu/criu/pie/restorer.c`, `criu/criu/sigframe.c`, `criu/criu/arch/aarch64/crtools.c`, `criu/criu/arch/aarch64/include/asm/restorer.h`, `criu/compel/arch/aarch64/src/lib/include/uapi/asm/sigframe.h`, `docs/A3-问题与解决方法复盘.md`, and `docs/04-Dev-Environment.md`.

## Global implementation rules

- Do not make the kernel parse protobuf or emit protobuf.
- Do not call unexported `mm_alloc()`, `do_mmap()`, `do_munmap()`, or `mremap_to()` from the existing out-of-tree module.
- The new kernel functionality is a Linux 5.10.29 source patch, not an implementation hidden in the existing debugfs module.
- `VALIDATE` copies the complete plan and VMA array; `COMMIT` never dereferences a userspace pointer.
- `COMMIT` is one-way. Before it starts, return a diagnostic errno; after it starts, terminate and clean the carrier by recorded PID.
- `target_pid` is part of the plan and `COMMIT` must reject callers whose current PID does not match it.
- Use guest-local `/tmp` for images, restore workspaces, logs, and binaries. Do not use the Lima 9p project path as the restore working directory.
- Every end-to-end claim requires restored PID liveness, continuing behavior, memory markers, maps, and clean guest dmesg; an exit code of zero is insufficient.
- Do not add unsupported features silently. Return `-EOPNOTSUPP` before the irreversible boundary.

### Task 1: Lock the restore UAPI and transaction contract

**Files:**

- Create: `include/criu_restore_abi.h`
- Create: `tests/b1-restore-abi-contract.sh`
- Create: `tests/fixtures/b1-restore-plan-builder.py`
- Modify: `docs/superpowers/specs/2026-09-19-b1-kernel-assisted-restore-design.md`

**Step 1: Write failing ABI tests**

Add a Python fixture builder and shell contract that assert:

- `CRIU_RESTORE_ABI_VERSION == 1`;
- `struct criu_restore_plan_v1` has fixed-width fields, includes `target_pid`, and has no
  variable-size inline data;
- VMA records are page-aligned and bounded;
- unknown versions, undersized structures, zero `target_pid`, duplicate targets, overflowed
  ranges, and more than `CRIU_RESTORE_MAX_VMAS` are rejected;
- the contract distinguishes `VALIDATE`’s `vmas_user_ptr` from all `COMMIT` inputs.

Run:

```sh
sh tests/b1-restore-abi-contract.sh
```

Expected: FAIL because the UAPI header and validator do not exist.

**Step 2: Define the UAPI**

Add ioctl numbers and packed, fixed-width structures for:

- `CRIU_RESTORE_VALIDATE_V1`;
- `CRIU_RESTORE_COMMIT_V1`;
- anonymous private, file-private, stack, and vDSO VMA kinds;
- plan flags and per-VMA flags;
- `target_pid`, bootstrap ranges, staging/final sigframe SP, and TLS.

Document that `vmas_user_ptr` is valid only during `VALIDATE`, and that the kernel transaction
owns its copied plan afterward.

**Step 3: Run the ABI contract and existing regressions**

Run:

```sh
sh tests/b1-restore-abi-contract.sh
sh tests/snapshot-format.sh
sh tests/a7-unsupported.sh
```

Expected: all PASS, with no changes to A3-A8 snapshot ABI behavior.

**Step 4: Commit**

```sh
git add include/criu_restore_abi.h tests/b1-restore-abi-contract.sh \
  tests/fixtures/b1-restore-plan-builder.py \
  docs/superpowers/specs/2026-09-19-b1-kernel-assisted-restore-design.md
git commit -m "docs: lock B1 restore transaction ABI"
```

### Task 2: Add the Linux 5.10.29 restore-device patch scaffold

**Files:**

- Create: `patches/linux-5.10.29/0004-criu-restore-mm-helper.patch`
- Modify: `scripts/apply-kernel-patches.sh`
- Create: `tests/b1-kernel-patch-contract.sh`

**Step 1: Write the patch contract**

Assert that the patch:

- adds a dedicated misc device implementation in the pinned kernel tree;
- registers `/dev/criu_restore`;
- contains both ioctl handlers;
- does not modify the existing A2/A7 freezer semantics;
- is idempotently applied by `scripts/apply-kernel-patches.sh`.

Run it before implementation and expect FAIL.

**Step 2: Add the patch scaffold**

Patch Linux 5.10.29 with a dedicated source file under `kernel/` and the required
`kernel/Makefile` entry. The device must use a per-open transaction object with explicit
states:

```text
OPEN -> VALIDATED -> COMMITTING -> COMMITTED
                         \-------> FAILED
```

Require `CAP_SYS_ADMIN` for opening and ioctl operations. Store the opener’s credentials for
diagnostics, but authorize `COMMIT` by the copied `target_pid` and the current task’s PID.

**Step 3: Update patch application**

Make `scripts/apply-kernel-patches.sh` verify/apply patch 0004 after the existing freezer
patches and print a distinct `B1_RESTORE: PATCH_APPLIED` or `PATCH_ALREADY_APPLIED` marker.

**Step 4: Run the patch contract**

Run:

```sh
sh tests/b1-kernel-patch-contract.sh
```

Expected: PASS on a clean Linux 5.10.29 tree and PASS again on a second idempotent run.

**Step 5: Commit**

```sh
git add patches/linux-5.10.29/0004-criu-restore-mm-helper.patch \
  scripts/apply-kernel-patches.sh tests/b1-kernel-patch-contract.sh
git commit -m "build: add Linux 5.10.29 restore helper patch"
```

### Task 3: Implement `VALIDATE` with TOCTOU-safe plan copying

**Files in the kernel patch:**

- Modify: `kernel/criu_restore.c`
- Modify: `include/criu_restore_abi.h` if the kernel include path needs a UAPI mirror

**Files in the project tests:**

- Create: `tests/b1-validate-contract.sh`
- Modify: `tests/fixtures/b1-restore-plan-builder.py`

**Step 1: Write negative validation tests**

Cover:

- bad ioctl size/version;
- invalid `target_pid`;
- VMA count zero/over-limit;
- unaligned start/length;
- integer overflow;
- duplicate target intervals;
- bootstrap or sigframe outside declared reserved ranges;
- unsupported VMA kind or flags;
- second `VALIDATE`;
- modifying the userspace VMA array after `VALIDATE` and before `COMMIT`.

Expected before implementation: FAIL.

**Step 2: Copy and validate**

Implement `copy_from_user()` for the fixed plan followed by a bounded allocation and
`copy_from_user()` of the complete VMA array. Copy all values into transaction-owned memory.
Do not retain a userspace pointer for later use.

Check:

- ABI version/size/flags;
- `target_pid`;
- page alignment and overflow;
- target uniqueness and supported ranges;
- bootstrap code/stack/sigframe containment;
- staging and target interval metadata;
- `current->mm` is present when validation is issued;
- transaction state is `OPEN`.

**Step 3: Verify TOCTOU behavior**

Run:

```sh
sh tests/b1-validate-contract.sh
```

Expected: invalid plans fail without changing the carrier address space; a userspace mutation
after validation does not change the kernel’s saved plan.

**Step 4: Commit**

```sh
git add tests/b1-validate-contract.sh tests/fixtures/b1-restore-plan-builder.py
git commit -m "feat: validate and snapshot restore plans atomically"
```

The patch file is already tracked by Task 2; amend only if the patch itself changed.

### Task 4: Implement kernel `COMMIT` VMA transaction

**Files in the kernel patch:**

- Modify: `kernel/criu_restore.c`
- Modify: Linux 5.10.29 `mm/mmap.c`, `mm/mremap.c`, or a helper source included by the patch

**Files in the project tests:**

- Create: `tests/b1-vma-commit-contract.sh`
- Create: `tests/fixtures/b1-overlap-plan.c`

**Step 1: Write the commit contract**

Exercise:

- a non-overlapping source-to-target move;
- source/target overlap requiring a guard page and temporary location;
- low-to-high and high-to-low batches;
- bootstrap and staging regions that must remain mapped;
- repeated `COMMIT`;
- commit from a non-target PID;
- commit after a failed validation.

Expected before implementation: FAIL.

**Step 2: Implement the commit state machine**

On `CRIU_RESTORE_COMMIT_V1`:

1. require `VALIDATED`, `CAP_SYS_ADMIN`, and `task_pid_nr(current) == target_pid`;
2. transition to `COMMITTING` before the first irreversible operation;
3. operate only on the transaction-owned plan;
4. preserve bootstrap and staging ranges;
5. unmap old ranges with the equivalent of CRIU `unmap_old_vmas()`;
6. process VMA moves in the same directional order as CRIU `vma_remap()`;
7. for overlapping source/target ranges, install a guard page, move source to a temporary
   non-overlapping range, then move to target;
8. restore final `prot` and supported map flags;
9. leave bootstrap mapped until the userspace `rt_sigreturn` path has started;
10. transition to `COMMITTED` only after every VMA move succeeds.

The patch must use the pinned 5.10.29 internal APIs in-tree, with version-specific wrappers
if needed. It must not export those APIs to the old out-of-tree module.

**Step 3: Verify state and failure behavior**

Run:

```sh
sh tests/b1-vma-commit-contract.sh
```

Expected: all pre-commit failures preserve the old address space; post-commit failures cause
the carrier to exit and leave the transaction non-reusable.

**Step 4: Commit**

```sh
git add tests/b1-vma-commit-contract.sh tests/fixtures/b1-overlap-plan.c \
  patches/linux-5.10.29/0004-criu-restore-mm-helper.patch
git commit -m "feat: commit staged VMAs through kernel restore transaction"
```

### Task 5: Create the userspace CRIU image reader and support validator

**Files:**

- Create: `userspace/mini-restore/Makefile`
- Create: `userspace/mini-restore/restore.h`
- Create: `userspace/mini-restore/image_reader.c`
- Create: `userspace/mini-restore/image_reader.h`
- Create: `userspace/mini-restore/validator.c`
- Create: `userspace/mini-restore/validator.h`
- Create: `tests/b1-image-reader-contract.sh`
- Create: `tests/fixtures/b1-image-builder.py`

**Step 1: Write image-reader tests**

Use real CRIU image fixtures where available and synthetic malformed images for:

- missing required image;
- wrong architecture;
- missing core/mm/vma/pagemap;
- unsupported threads, children, shared mm, namespaces, shared mappings, dirty file-private
  pages, PAC/SVE/GCS, or vDSO relocation;
- page-count and address overflow;
- invalid protobuf field combinations.

Expected before implementation: FAIL.

**Step 2: Implement the model**

Use CRIU’s generated protobuf-c definitions and the existing CRIU image magic/length framing.
Keep the model independent from `criu-module-convert`; do not teach the kernel about protobuf.
Represent:

- target PID and a single task/core;
- mm and ordered VMA records;
- page runs and page-image references;
- ordinary file-backed records needed to reopen clean file-private mappings;
- aarch64 registers, signal mask, FPSIMD, and TLS;
- bootstrap/staging metadata.

**Step 3: Implement support validation**

Validate the B1 matrix from the spec before opening the carrier:

- one task and one thread;
- no child/pstree dependency;
- supported VMA kinds only;
- no dirty file-private pages;
- no shared memory or shared file mappings;
- no vDSO relocation;
- page-aligned non-overlapping target VMAs;
- stable file identity and sufficient file size.

Return distinct diagnostics for `FORMAT`, `UNSUPPORTED`, and `IO` failures.

**Step 4: Run tests**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b1-image-reader-contract.sh
```

Expected: PASS, and all legacy converter tests remain PASS.

**Step 5: Commit**

```sh
git add userspace/mini-restore tests/b1-image-reader-contract.sh \
  tests/fixtures/b1-image-builder.py
git commit -m "feat: parse and validate supported CRIU restore images"
```

### Task 6: Implement carrier creation and pre-commit resource preparation

**Files:**

- Create: `userspace/mini-restore/carrier.c`
- Create: `userspace/mini-restore/carrier.h`
- Create: `userspace/mini-restore/files.c`
- Create: `userspace/mini-restore/files.h`
- Create: `tests/b1-carrier-contract.sh`

**Step 1: Write carrier tests**

Cover:

- `clone3(set_tid)` with `set_tid_size = 1`;
- target PID mismatch;
- PID already occupied;
- parent records the carrier and waits with `waitpid`;
- child failure before `COMMIT` is reported and cleaned.

Expected before implementation: FAIL.

**Step 2: Implement clone3**

Build `struct clone_args` with the target PID, no `CLONE_VM`, no `CLONE_THREAD`, and an
explicit child stack. Keep the parent-side list of created PIDs for cleanup. Treat
`EEXIST`, `EPERM`, and `EINVAL` as diagnostic restore failures.

**Step 3: Implement supported file preparation**

Before commit, reopen only ordinary file-backed mappings and fd 0/1/2 as required by the
core gate. Verify device/inode/size with `fstat`; do not silently substitute a path whose
identity changed. Set descriptor flags and offsets before the address-space commit.

**Step 4: Run tests**

Run:

```sh
make -C userspace/mini-restore all
sh tests/b1-carrier-contract.sh
```

Expected: PASS, with every child failure leaving no live carrier.

**Step 5: Commit**

```sh
git add userspace/mini-restore/carrier.* userspace/mini-restore/files.* \
  tests/b1-carrier-contract.sh
git commit -m "feat: create exact-PID carrier and prepare restore files"
```

### Task 7: Implement staging VMA creation and page population

**Files:**

- Create: `userspace/mini-restore/staging.c`
- Create: `userspace/mini-restore/staging.h`
- Create: `tests/b1-staging-contract.sh`

**Step 1: Write staging tests**

Cover:

- anonymous private VMA;
- clean file-private VMA;
- page-aligned staging allocation;
- staging/target non-overlap;
- guard-page reservation for grow-down stack;
- zero-page and absent-page handling;
- `pread` page population without mapping the image file into the restore address space.

Expected before implementation: FAIL.

**Step 2: Implement CRIU-compatible staging**

Mirror CRIU `prepare_mappings()` and `premap_private_vma()`:

- reserve one staging arena;
- map anonymous VMA content anonymously;
- map clean file-private VMA content from the validated fd;
- temporarily add `PROT_WRITE` only when page population needs it;
- fill page runs using page-index-to-byte-offset conversion;
- never fault absent pages merely to dump/restore them;
- track each staging range and final target range in the restore plan.

Reject dirty file-private pages and unsupported special mappings before validation.

**Step 3: Run tests**

Run:

```sh
sh tests/b1-staging-contract.sh
```

Expected: PASS, with target address ranges untouched until kernel `COMMIT`.

**Step 4: Commit**

```sh
git add userspace/mini-restore/staging.* tests/b1-staging-contract.sh
git commit -m "feat: stage supported CRIU VMAs and page contents"
```

### Task 8: Implement aarch64 sigframe, TLS, and bootstrap

**Files:**

- Create: `userspace/mini-restore/sigframe.c`
- Create: `userspace/mini-restore/sigframe.h`
- Create: `userspace/mini-restore/bootstrap.S`
- Create: `userspace/mini-restore/bootstrap.h`
- Create: `tests/b1-sigframe-contract.sh`

**Step 1: Write sigframe tests**

Assert:

- 16-byte alignment;
- 31 general registers plus SP/PC/PSTATE;
- signal mask placement;
- FPSIMD magic/size;
- 32 SIMD registers, FPSR, and FPCR;
- TLS is kept separately from the sigframe;
- bootstrap contains no libc calls or old-stack accesses.

Expected before implementation: FAIL.

**Step 2: Implement the exact CRIU/Linux layout**

Reuse the aarch64 layout from `criu/compel/.../sigframe.h` and the population semantics from
`criu/criu/arch/aarch64/crtools.c`. Do not invent a shortened frame. Place the frame in the
final stack VMA, but keep its staging address available until `COMMIT` completes.

**Step 3: Implement bootstrap**

The bootstrap must:

1. receive the kernel transaction fd and copied scalar arguments from a fixed bootstrap
   structure;
2. issue `CRIU_RESTORE_COMMIT_V1`;
3. set `sp` to `sigframe_final_sp`;
4. execute `msr tpidr_el0, tls`;
5. load `__NR_rt_sigreturn` into `x8` and issue `svc #0`;
6. never return to C.

The bootstrap must remain position-independent and must not call libc.

**Step 4: Run tests**

Run:

```sh
make -C userspace/mini-restore all
sh tests/b1-sigframe-contract.sh
```

Expected: PASS, with assembly inspection confirming the required syscall and TLS sequence.

**Step 5: Commit**

```sh
git add userspace/mini-restore/sigframe.* userspace/mini-restore/bootstrap.* \
  tests/b1-sigframe-contract.sh
git commit -m "feat: restore aarch64 sigframe through bootstrap rt_sigreturn"
```

### Task 9: Implement orchestrator and pre/post-commit cleanup

**Files:**

- Create: `userspace/mini-restore/main.c`
- Create: `userspace/mini-restore/cleanup.c`
- Create: `userspace/mini-restore/cleanup.h`
- Modify: `userspace/mini-restore/Makefile`
- Create: `tests/b1-cleanup-contract.sh`

**Step 1: Write cleanup tests**

Cover:

- parser failure before carrier creation;
- carrier failure before `COMMIT`;
- `VALIDATE` failure;
- `COMMIT` failure after address-space mutation;
- carrier already changed session/pgid;
- repeated restore after failure with the same target PID.

Expected before implementation: FAIL.

**Step 2: Implement the state machine**

Implement this exact userspace order:

```text
read images
validate all support/identity rules
open /dev/criu_restore
VALIDATE (plan copied into kernel)
clone3(set_tid)
prepare supported fds and auxiliary state
create staging VMAs and fill pages
construct final sigframe and bootstrap
jump to bootstrap
COMMIT
msr tpidr_el0
rt_sigreturn
```

Keep a PID cleanup table from the moment `clone3` succeeds. Before the irreversible boundary,
unmap staging and close resources. After `COMMIT` begins, kill each recorded PID and
`waitpid()` it on any observable failure; do not rely on process-group cleanup.

**Step 3: Run unit/contract tests**

Run:

```sh
make -C userspace/mini-restore clean all
sh tests/b1-cleanup-contract.sh
sh tests/b1-restore-abi-contract.sh
sh tests/b1-image-reader-contract.sh
```

Expected: PASS.

**Step 4: Commit**

```sh
git add userspace/mini-restore tests/b1-cleanup-contract.sh
git commit -m "feat: orchestrate transactional single-process restore"
```

### Task 10: Add real CRIU dump fixture and guest-local smoke gate

**Files:**

- Create: `tests/progs/b1-minimal.c`
- Modify: `tests/progs/Makefile`
- Create: `tests/b1-kernel-assisted-restore.sh`
- Create: `tests/b1-guest-helpers.sh`
- Modify: `scripts/run-qemu.sh` only if the existing staging contract cannot run the gate
- Modify: `docs/steps/B1-mini-restore.md`

**Step 1: Write the fixture**

The fixture must print its PID and continuously update a tick, while retaining stable heap and
stack markers and a TLS marker. It must be static/minimal and avoid threads, children,
namespaces, shared memory, sockets, timers, and dirty file-private mappings.

**Step 2: Write the guest script**

The script must:

1. run in the Linux 5.10.29 guest;
2. stage all work below guest-local `/tmp`;
3. build/load the patched kernel and the mini-restore binary as provided by the existing
   Lima/QEMU workflow;
4. start the fixture and record PID/tick/markers/maps/dmesg baseline;
5. dump with the real CRIU;
6. terminate the original process and confirm its PID is free;
7. restore with `userspace/mini-restore`;
8. verify exact PID, liveness, tick growth, markers, TLS behavior, and maps;
9. inspect dmesg for Oops/BUG/WARNING/refcount/KASAN;
10. clean every carrier and temporary directory.

The only success marker is:

```text
B1_KERNEL_ASSISTED_RESTORE: PASS
```

**Step 3: Run the gate**

From macOS, invoke the documented Lima wrapper:

```sh
limactl shell criu-dev bash -lc \
  'cd /Users/yhome/workspace/source_code/criu_module && \
   ./scripts/run-qemu.sh --ci --script tests/b1-kernel-assisted-restore.sh'
```

Expected: the guest prints the PASS marker and exits zero; a missing marker, dead restored
PID, unchanged tick, dirty dmesg, or leaked carrier exits nonzero.

**Step 4: Commit**

```sh
git add tests/progs/b1-minimal.c tests/progs/Makefile \
  tests/b1-kernel-assisted-restore.sh tests/b1-guest-helpers.sh \
  docs/steps/B1-mini-restore.md
git commit -m "test: add B1 kernel-assisted guest restore gate"
```

### Task 11: Add negative guest gates and patch/build regression coverage

**Files:**

- Create: `tests/b1-negative.sh`
- Create: `tests/b1-patch-build.sh`
- Modify: `ci/` workflow only if the project’s existing CI can build the pinned guest patch
- Modify: `docs/steps/B1-mini-restore.md`

**Step 1: Add negative cases**

Require explicit `-EOPNOTSUPP` or format errors for:

- dirty file-private VMA;
- vDSO relocation;
- shared mapping;
- multi-thread image;
- child/process-tree image;
- unsupported namespace/cgroup/fs restore metadata;
- invalid backing-file identity;
- target PID occupied;
- duplicate commit.

**Step 2: Add build regression**

Verify:

- patch application to a clean Linux 5.10.29 tree;
- second patch application is idempotent;
- kernel build;
- existing A2/A7 freezer tests still pass;
- existing A3-A8 converter and guest gates still pass.

**Step 3: Run**

```sh
sh tests/b1-negative.sh
sh tests/b1-patch-build.sh
sh tests/a5-converter-fds.sh
sh tests/a7-unsupported.sh
sh tests/a8-shared-fdtable.sh
```

Expected: all PASS; unsupported cases never reach `COMMIT`.

**Step 4: Commit**

```sh
git add tests/b1-negative.sh tests/b1-patch-build.sh \
  docs/steps/B1-mini-restore.md
git commit -m "test: cover B1 unsupported cases and kernel regressions"
```

### Task 12: Final verification and documentation handoff

**Files:**

- Modify: `docs/steps/B1-mini-restore.md`
- Modify: `docs/03-Iteration-Plan.md`
- Create: `docs/plans/2026-09-19-b1-verification.md`

**Step 1: Run the complete local contract set**

```sh
git diff --check
make -C userspace/mini-restore clean all
sh tests/b1-restore-abi-contract.sh
sh tests/b1-kernel-patch-contract.sh
sh tests/b1-validate-contract.sh
sh tests/b1-vma-commit-contract.sh
sh tests/b1-image-reader-contract.sh
sh tests/b1-carrier-contract.sh
sh tests/b1-staging-contract.sh
sh tests/b1-sigframe-contract.sh
sh tests/b1-cleanup-contract.sh
sh tests/b1-negative.sh
```

**Step 2: Run the authoritative guest gate**

Run `tests/b1-kernel-assisted-restore.sh` through `limactl shell criu-dev` and
`scripts/run-qemu.sh`, then preserve:

- exact command;
- guest kernel/config identity;
- restored PID and behavior evidence;
- maps comparison;
- dmesg before/after;
- patch hash and userspace binary hash.

**Step 3: Write the verification record**

Record PASS/FAIL for each gate, unsupported matrix, cleanup evidence, and any deferred
capability. Do not mark B1 complete if only the userspace binary builds or if CRIU exits zero
without a living restored PID.

**Step 4: Update status documents**

Only after fresh evidence:

- mark the B1 design as “implementation plan generated” or “implemented” as appropriate;
- update `docs/steps/B1-mini-restore.md` with actual supported scope;
- update `docs/03-Iteration-Plan.md` only for completed gates;
- preserve all deferred features as explicit follow-up tasks.

**Step 5: Commit**

```sh
git add docs/steps/B1-mini-restore.md docs/03-Iteration-Plan.md \
  docs/plans/2026-09-19-b1-verification.md
git commit -m "docs: record B1 kernel-assisted restore verification"
```

## Execution notes

- This plan is intentionally separate from the design contract. Use
  `superpowers:executing-plans` in a new implementation session after the user chooses the
  execution mode.
- Do not merge or push during implementation until the verification-before-completion skill
  has fresh guest evidence.
- Do not commit `artifacts/`, generated guest binaries, QEMU staging directories, or local
  kernel build outputs.
