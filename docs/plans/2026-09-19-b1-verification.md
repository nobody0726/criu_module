# B1 kernel-assisted restore verification

Date: 2026-09-19

## Verdict

B1 is **not complete**.

The local contract layer is implemented and passing, but the authoritative Linux 5.10.29
guest restore gate has not produced `B1_KERNEL_ASSISTED_RESTORE: PASS`.

The first parser blocker is resolved: `userspace/mini-restore` now auto-detects CRIU v1.1
framing and parses the supported subset of real CRIU protobuf `inventory`, `pstree`, `core`,
`mm`, and `pagemap` records in userspace. The synthetic manifest path remains for focused
contract tests. The authoritative guest gate still has not produced a PASS marker.

## Passing local contracts

- `git diff --check`
- `make -C userspace/mini-restore clean all`
- `sh tests/b1-restore-abi-contract.sh`
- `KDIR=/tmp/criu-module-b1-no-kernel sh tests/b1-kernel-patch-contract.sh`
- `sh tests/b1-validate-contract.sh`
- `sh tests/b1-vma-commit-contract.sh`
- `sh tests/b1-image-reader-contract.sh`
- `sh tests/b1-carrier-contract.sh`
- `sh tests/b1-staging-contract.sh`
- `sh tests/b1-sigframe-contract.sh`
- `sh tests/b1-cleanup-contract.sh`
- `sh tests/b1-negative.sh`
- `sh tests/b1-patch-build.sh`
- `sh tests/b1-qemu-staging-contract.sh`

Additional parser evidence:

- real-image wire fixture accepted by `tests/b1-image-reader-contract.sh`;
- existing CRIU image set under `artifacts/s1/.../img` accepted by `mini-restore --dry-run`
  with the recorded target PID and VMA/page model.
- `tests/b1-kernel-assisted-restore.sh` now treats real-image `--dry-run` acceptance as a
  prerequisite and separately reports missing `/dev/criu_restore`; it no longer expects the
  real protobuf image to be rejected.

## Verified implementation slices

- Restore transaction ABI with VALIDATE-only user VMA pointer and pointer-free COMMIT.
- Linux 5.10.29 patch scaffold and source-level COMMIT transaction helpers.
- Userspace-only model/validator boundary; no kernel protobuf parsing.
- Exact-PID carrier scaffold with recorded-PID cleanup.
- Staging VMA/page population contracts.
- aarch64 sigframe model and bootstrap assembly contract.
- Orchestrator dry-run and cleanup contracts.
- Negative unsupported/format/IO diagnostics at the current contract layer.

## Not yet verified

- Full real-image support for file identity reopening and compressed/parent page runs.
- Linux 5.10.29 guest kernel build of patch 0004 after full application.
- Live `/dev/criu_restore` VALIDATE/COMMIT ioctl path in the guest.
- Exact PID restore liveness after COMMIT.
- Tick growth, heap/stack/TLS marker preservation, and `/proc/$pid/maps` comparison.
- Clean guest `dmesg` after a successful restore.
- Live-kernel-only negatives: target PID occupied and duplicate COMMIT.

## Guest environment blocker

The Lima `criu-dev` guest was reached before the final gate attempt, but its root ext4
filesystem had an aborted journal and was repeatedly remounted read-only. This made both
`/tmp` and `/home/yhome.guest` unwritable; the worktree build failed before compilation with
`Cannot create temporary file in /tmp/: Read-only file system`. A stop/start did not restore
SSH, so the instance was force-stopped without deleting its disk. No guest PASS/FAIL result is
inferred from this incident.

## Required next work

1. Complete file-backed VMA identity reopening and unsupported-page handling for the parsed
   real-image model.
2. Re-run `tests/b1-kernel-assisted-restore.sh` through:

   ```sh
   limactl shell criu-dev bash -lc \
     'cd /Users/yhome/workspace/source_code/criu_module && \
      ./scripts/run-qemu.sh --ci --script tests/b1-kernel-assisted-restore.sh'
   ```

3. Only mark B1 complete if the guest prints exactly:

   ```text
   B1_KERNEL_ASSISTED_RESTORE: PASS
   ```

   and the evidence includes restored PID liveness, tick growth, markers/TLS/maps, and clean dmesg.
