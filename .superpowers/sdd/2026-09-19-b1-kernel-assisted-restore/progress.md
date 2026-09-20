# SDD ledger — plan: docs/plans/2026-09-19-b1-kernel-assisted-restore.md

## Preflight plan scan

| Scope | Files/interfaces shared | Finding | Ruling |
|---|---|---|---|
| Task 1 → Task 3 | `include/criu_restore_abi.h`, plan validation fields | Task 1 defines the ABI; Task 3 consumes it for kernel copying and validation. | Task 1 is authoritative for field names and sizes; Task 3 may add only validation helpers, not new pointer-bearing inputs. |
| Task 2 → Task 3/4 | `0004-criu-restore-mm-helper.patch`, `/dev/criu_restore` | Task 2 creates the patch scaffold; Tasks 3/4 extend the same patch. | Keep one patch file and one kernel source path; later tasks amend the patch, never create a competing device. |
| Task 3 → Task 4 | transaction state and copied VMA plan | Task 4 depends on Task 3's `OPEN → VALIDATED` state and transaction-owned plan. | `COMMIT` must never access userspace memory; all move metadata comes from the copied plan. |
| Task 4 → Task 8/9 | ioctl commit and bootstrap | Task 8's bootstrap must call the exact ioctl ABI implemented by Task 4. | Keep ioctl scalar arguments in a fixed bootstrap structure; `target_pid` authorization is checked in kernel. |
| Task 5 → Task 6/7/8 | restore model and validator outputs | Later tasks require one stable model for VMA/page/core/TLS data. | Task 5 owns model layout; later tasks add execution state without reparsing protobuf. |
| Task 6 → Task 9 | carrier PID and cleanup table | Task 9 cleans every PID created by Task 6. | Carrier API must expose recorded PID and wait status; no process-group-only cleanup. |
| Task 7 → Task 4/9 | staging ranges and plan VMA array | Userspace supplies staging/target ranges to kernel. | Staging must remain target-nonoverlapping and be represented exactly in the validated plan. |
| Task 8 → Task 4/9 | final sigframe SP, TLS, bootstrap | Kernel must preserve bootstrap and final stack until `rt_sigreturn`. | Bootstrap is independent code; TLS is set immediately before `svc`, not delegated to sigreturn. |
| Task 10 → Task 11/12 | guest gate and diagnostics | Negative and regression tests depend on the authoritative guest script. | Guest-local `/tmp`, restored PID liveness, behavior markers, maps, and dmesg are mandatory evidence. |
| Every task | A3/A8 review constraints | No task may move protobuf parsing into kernel or claim success from exit code alone. | Reject scope drift; unsupported cases return explicit errors. |

| Task | Self-consistency check | Ruling |
|---|---|---|
| 1 | ABI tests and header changes align. | Proceed. |
| 2 | Patch contract, apply script, and patch scaffold align. | Proceed; patch must be idempotent. |
| 3 | Negative validation cases exercise the fields Task 1 defines. | Proceed. |
| 4 | Commit tests exercise the state machine Task 3 produces. | Proceed. |
| 5 | Image reader files and tests are self-contained. | Proceed; use CRIU source as oracle, not kernel ABI. |
| 6 | Carrier/files APIs feed Task 9 cleanup. | Proceed. |
| 7 | Staging APIs feed Task 4 plan generation. | Proceed. |
| 8 | Sigframe/bootstrap APIs feed Task 9 orchestration. | Proceed. |
| 9 | Orchestrator order matches spec and cleanup contract. | Proceed. |
| 10 | Fixture and guest gate cover the core acceptance matrix. | Proceed. |
| 11 | Negative and build regressions do not broaden core scope. | Proceed. |
| 12 | Verification records only fresh evidence. | Proceed. |

## Rulings

- Ruling: execute in the isolated worktree `/Users/yhome/.codex/worktrees/b1-kernel-assisted-restore/criu_module` — the user selected current-session execution, while the skill requires implementation isolation.
- Ruling: preserve `artifacts/` as an untracked user directory — it predates B1 and is outside this plan.
- Ruling: implementation starts with Task 1 and remains task-serial — kernel patch and userspace APIs share ABI surfaces, so parallel implementation would create avoidable conflicts.
- Ruling: Task 1 implementation was completed locally after two implementer agents failed/stalled at the tool/model layer — this preserves TDD and plan progress; cost if wrong is reduced independent review coverage for Task 1.
- Ruling: Task 1 local equivalent review was used while waiting, but the independent reviewer later returned APPROVED before Task 2 began — no remaining cost.

## Task 1

- Base: `fb366d7`
- Commit: `2fe94d8 docs: lock B1 restore transaction ABI`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-1-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-fb366d7..2fe94d8.diff`
- Independent review: APPROVED. Checked fixed-width structures, no embedded user pointers in COMMIT, `target_pid` present, `vmas_user_ptr` documented as VALIDATE-only, Linux/non-Linux type handling, no protobuf dependency in ABI, and no A3-A8 snapshot regression.
- Tests: `sh tests/b1-restore-abi-contract.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 1: complete.

## Task 2

- Base: `2fe94d8`
- Commit: `33f11f2 build: add Linux 5.10.29 restore helper patch`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-2-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-2fe94d8..33f11f2.diff`
- Independent review: NOT APPROVED initially. Findings were shell portability of the contract, invoking bash apply script via `sh`, and weak PID-authorization grep.
- Fix round 1: resolved all three by making the contract POSIX-`sh` compatible, invoking `bash "$apply"`, and grepping the real `task_pid_nr(current) != tx->plan.target_pid` expression.
- Scoped re-review: APPROVED. Confirmed `sh` portability path, `bash "$apply"` invocation, and real PID authorization grep.
- Tests: `sh tests/b1-kernel-patch-contract.sh`; `dash tests/b1-kernel-patch-contract.sh`; `sh tests/b1-restore-abi-contract.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Note: an extra compile preflight failed because copying the full kernel tree exhausted macOS temp volume. The explicitly created temporary kernel-copy directories were deleted afterward; this did not affect project files.
- Task 2: complete.

## Task 3

- Base: `33f11f2`
- Commit: `83b32ae feat: validate and snapshot restore plans atomically`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-3-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-33f11f2..83b32ae.diff`
- Local review: APPROVED for continuing to Task 4. Checked that VALIDATE copies plan and VMA array with `copy_from_user`, rejects bad versions/sizes/PIDs/VMA counts/alignment/overflow/duplicates/unsupported kind/flags/bootstrap/sigframe containment, stores transaction-owned `tx->plan` and `tx->vmas`, and leaves COMMIT free of user VMA pointers.
- Tests: `sh tests/b1-validate-contract.sh`; `dash tests/b1-validate-contract.sh`; `KDIR=/tmp/criu-module-b1-no-kernel sh tests/b1-kernel-patch-contract.sh`; `sh tests/b1-restore-abi-contract.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`; local hunk-count checker for patch 0004.
- Note: skipped repeated full kernel-tree copy during Task 3 regression to avoid the A3/A8-style environment pitfall already observed in Task 2; the full idempotent patch apply path was verified in Task 2.
- Task 3: complete.

## Task 4

- Base: `83b32ae`
- Commit: `5483de2 feat: commit staged VMAs through kernel restore transaction`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-4-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-83b32ae..5483de2.diff`
- Local review: APPROVED for continuing to Task 5. Checked source-level contract for CRIU-style directional moves, overlap guard/temp handling, preserving staging/bootstrap while unmapping old ranges, final protection restore, target PID authorization, no COMMIT user VMA pointer read, and post-COMMIT failure SIGKILL.
- Tests: `sh tests/b1-vma-commit-contract.sh`; `dash tests/b1-vma-commit-contract.sh`; `sh tests/b1-validate-contract.sh`; `KDIR=/tmp/criu-module-b1-no-kernel sh tests/b1-kernel-patch-contract.sh`; `sh tests/b1-restore-abi-contract.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`; local hunk-count checker; `patch --dry-run -d /Users/yhome/workspace/source_code/linux-5.10.29 -p1 < patches/linux-5.10.29/0004-criu-restore-mm-helper.patch`.
- Note: no full kernel-tree copy was performed; dry-run used the existing Linux 5.10.29 tree read-only.
- Task 4: complete.

## Task 5

- Base: `5483de2`
- Commit: `1f5f398 feat: parse and validate supported CRIU restore images`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-5-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-5483de2..1f5f398.diff`
- Local review: APPROVED for continuing to Task 6. Checked userspace-only model boundary, no kernel protobuf dependency, diagnostic classes, B1 support matrix, VMA/page overflow validation, and legacy converter regression coverage.
- Tests: `make -C userspace/mini-restore clean all`; `sh tests/b1-image-reader-contract.sh`; `dash tests/b1-image-reader-contract.sh`; `LDFLAGS= make -C userspace clean all`; `LDFLAGS= sh tests/converter-format.sh`; `LDFLAGS= sh tests/converter-images.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Note: plain macOS `make -C userspace clean all` still fails due the pre-existing converter `-static` default; this is an environment/toolchain limitation, not a Task 5 regression.
- Follow-up commit: `a57490b feat: parse real CRIU restore images in userspace`.
- Follow-up commit: `27d65a9 fix: preserve parsed VMA protection and flags`.
- Follow-up tests: real CRIU v1.1 wire fixtures, historical `artifacts/s1/.../img`
  `mini-restore --dry-run`, and the complete local B1 contract set.
- Task 5: complete for the supported uncompressed single-process model; file identity
  reopening and compressed/parent page runs remain explicit unsupported/deferred cases.

## Task 6

- Base: `1f5f398`
- Commit: `e7e155d feat: create exact-PID carrier and prepare restore files`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-6-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-1f5f398..e7e155d.diff`
- Local review: APPROVED for continuing to Task 7. Checked clone3/set_tid contract, no CLONE_VM/CLONE_THREAD sharing, PID record/wait/cleanup path, Linux-only unsupported stubs for macOS, and file identity/size/offset/fd flag validation.
- Tests: `sh tests/b1-carrier-contract.sh`; `dash tests/b1-carrier-contract.sh`; `sh tests/b1-image-reader-contract.sh`; `sh tests/b1-vma-commit-contract.sh`; `LDFLAGS= make -C userspace clean all`; `LDFLAGS= sh tests/converter-format.sh`; `LDFLAGS= sh tests/converter-images.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 6: complete.

## Task 7

- Base: `e7e155d`
- Commit: `985ad39 feat: stage supported CRIU VMAs and page contents`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-7-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-e7e155d..985ad39.diff`
- Local review: APPROVED for continuing to Task 8. Checked staging/target separation, page-aligned staging, `pread` page population, clean file-private hook, grow-down stack flagging, absent page behavior, and no target mapping before COMMIT.
- Tests: `sh tests/b1-staging-contract.sh`; `dash tests/b1-staging-contract.sh`; `sh tests/b1-carrier-contract.sh`; `sh tests/b1-image-reader-contract.sh`; `sh tests/b1-vma-commit-contract.sh`; `LDFLAGS= make -C userspace clean all`; `LDFLAGS= sh tests/converter-format.sh`; `LDFLAGS= sh tests/converter-images.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 7: complete.

## Task 8

- Base: `985ad39`
- Commit: `7007bc3 feat: restore aarch64 sigframe through bootstrap rt_sigreturn`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-8-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-985ad39..7007bc3.diff`
- Local review: APPROVED for continuing to Task 9. Checked 16-byte aligned frame model, 31 GPRs/SP/PC/PSTATE, signal mask, FPSIMD magic/size/regs/FPSR/FPCR, TLS kept separately, bootstrap COMMIT ioctl, SP switch, TPIDR_EL0 write, rt_sigreturn syscall, and no libc/return path.
- Tests: `sh tests/b1-sigframe-contract.sh`; `dash tests/b1-sigframe-contract.sh`; `sh tests/b1-staging-contract.sh`; `sh tests/b1-carrier-contract.sh`; `sh tests/b1-image-reader-contract.sh`; `sh tests/b1-vma-commit-contract.sh`; `LDFLAGS= make -C userspace clean all`; `LDFLAGS= sh tests/converter-format.sh`; `LDFLAGS= sh tests/converter-images.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 8: complete.

## Task 9

- Base: `7007bc3`
- Commit: `13e6b09 feat: orchestrate transactional single-process restore`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-9-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-7007bc3..13e6b09.diff`
- Local review: APPROVED for continuing to Task 10. Checked CLI state machine, dry-run gate, cleanup table, recorded-PID kill/wait cleanup, staging/fd cleanup, no process-group cleanup, and no kernel protobuf dependency.
- Tests: `sh tests/b1-cleanup-contract.sh`; `dash tests/b1-cleanup-contract.sh`; `sh tests/b1-restore-abi-contract.sh`; `sh tests/b1-image-reader-contract.sh`; `sh tests/b1-sigframe-contract.sh`; `sh tests/b1-staging-contract.sh`; `sh tests/b1-carrier-contract.sh`; `sh tests/b1-vma-commit-contract.sh`; `LDFLAGS= make -C userspace clean all`; `LDFLAGS= sh tests/converter-format.sh`; `LDFLAGS= sh tests/converter-images.sh`; `sh tests/snapshot-format.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 9: complete.

## Task 10

- Base: `13e6b09`
- Commit: `28cc481 test: add B1 guest restore gate scaffold`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-10-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-13e6b09..28cc481.diff`
- Status: scaffold plus real-image parser integration; not full PASS. Added guest-local `/tmp`
  fixture/gate plumbing and a userspace CRIU v1.1 wire reader for inventory/pstree/core/mm/
  pagemap. Full Task 10 remains blocked on live file-backed restore/bootstrap/kernel evidence.
- Tests: `sh -n tests/b1-kernel-assisted-restore.sh`; `sh -n tests/b1-guest-helpers.sh`; `sh tests/b1-cleanup-contract.sh`; `sh tests/b1-image-reader-contract.sh`; `make -C tests/progs clean b1-minimal LDFLAGS=`; `git diff --check`.
- A3/A8 ruling: do not emit `B1_KERNEL_ASSISTED_RESTORE: PASS` until the Linux 5.10.29 guest verifies exact PID liveness, tick growth, markers/TLS/maps, and clean dmesg.
- Task 10: incomplete; continue with real CRIU protobuf reader/live guest validation before marking complete.
- Follow-up commit: `5e749fc test: rebuild mini-restore in QEMU staging tree`.
- `scripts/run-qemu.sh` now rebuilds `userspace/mini-restore` inside the Linux staging
  directory, preventing a host Mach-O binary from being passed into the guest.

## Task 11

- Base: `28cc481`
- Commit: `b9c1ae8 test: cover B1 unsupported cases and kernel regressions`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-11-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-28cc481..b9c1ae8.diff`
- Status: complete for current contract layer; live-kernel-only negatives deferred until Task 10 unblock.
- Tests: `sh tests/b1-negative.sh`; `dash tests/b1-negative.sh`; `sh tests/b1-patch-build.sh`; `dash tests/b1-patch-build.sh`; `sh tests/a7-unsupported.sh`; `git diff --check`.
- Task 11: complete within current non-live scope.

## Task 12

- Base: `b9c1ae8`
- Commit: `e3adf14 docs: record B1 kernel-assisted restore verification`
- Implementer report: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/task-12-report.md`
- Review package: `.superpowers/sdd/2026-09-19-b1-kernel-assisted-restore/review-b9c1ae8..e3adf14.diff`
- Status: verification handoff updated after real-image parser integration; B1 overall remains
  incomplete/blocked pending live patched-kernel guest PASS.
- Tests: full local contract set from Task 12; `git diff --check`; `make -C userspace/mini-restore clean`.
- Task 12: documentation updated; B1: not complete.

## Guest environment incident (2026-09-19)

- `limactl shell criu-dev` initially reached the Lima VM, but `/dev/vda1` had an aborted
  ext4 journal and was repeatedly remounted read-only. Evidence in the guest journal:
  `EXT4-fs error (device vda1): ext4_journal_check_start:83: ... Detected aborted journal`
  followed by `Remounting filesystem read-only`.
- This made `/tmp` and `/home/yhome.guest` unwritable, so the first worktree mini-restore
  build failed before compilation with `Cannot create temporary file in /tmp/: Read-only
  file system`.
- A normal stop/start was attempted; the VM then failed to bring SSH back (`port 22` never
  became available), so no guest kernel build or QEMU gate claim is recorded. The instance
  was force-stopped without deleting its disk. This is an environment blocker, not a B1
  PASS/FAIL result.
