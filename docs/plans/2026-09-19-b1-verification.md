# B1 kernel-assisted restore verification

Date: 2026-09-19

## Verdict

B1 is **not complete**.

The local contract layer is implemented and passing, but the authoritative Linux 5.10.29
guest restore gate has not produced `B1_KERNEL_ASSISTED_RESTORE: PASS`.

Primary blocker: `userspace/mini-restore` currently parses the synthetic B1 manifest fixtures
used by the contract tests, not real CRIU protobuf image files. Therefore the guest gate cannot
yet restore a real CRIU dump, and it must not emit a fake PASS marker.

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

- Real CRIU protobuf image parsing.
- Linux 5.10.29 guest kernel build of patch 0004 after full application.
- Live `/dev/criu_restore` VALIDATE/COMMIT ioctl path in the guest.
- Exact PID restore liveness after COMMIT.
- Tick growth, heap/stack/TLS marker preservation, and `/proc/$pid/maps` comparison.
- Clean guest `dmesg` after a successful restore.
- Live-kernel-only negatives: target PID occupied and duplicate COMMIT.

## Required next work

1. Replace or extend the Task 5 synthetic reader with real CRIU protobuf-c image parsing.
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
