# A5 文件描述符 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend the frozen kernel dump and converter with deduplicated regular-file fd records and explicit unsupported handling for pipe/socket state.

**Architecture:** Keep the snapshot TLV ABI little-endian and backward-readable. The kernel snapshots and pins every fd before sleeping, maps `struct file *` pointers to object IDs, and emits one extended FD record per descriptor. The userspace converter builds one CRIU file entry per object and one fdinfo entry per descriptor, while rejecting unsupported object types before emitting images.

**Tech Stack:** Linux 5.10.29 kernel module, C11 userspace converter, shell/Python contract fixtures, CRIU image protobuf wire format, Lima plus nested QEMU for the authoritative gate.

---

### Task 1: Add the failing A5 contract test

**Files:** Create `tests/a5-fd-contract.sh`.

1. Assert the extended FD ABI constants/fields, objmap declarations, flag sanitization, and type dispatch are present.
2. Assert fd-table callbacks do not perform image I/O while holding `file_lock` or an RCU read-side section.
3. Assert converter checks the extended record size and emits object-id based fdinfo.
4. Run `sh tests/a5-fd-contract.sh`; expect failure because the new symbols do not yet exist.
5. Commit the test.

### Task 2: Extend the snapshot FD ABI and converter model

**Files:** Modify `include/criu_snapshot.h`, `kernel_module/checkpoint/dump_files.h`, `userspace/criu-module-convert/criu_model.c`.

1. Append `object_id`, `type`, and `object_flags` after the existing 512-byte path so old 560-byte records remain readable.
2. Define `CRIU_SNAPSHOT_FD_RECORD_SIZE`, `CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE`, and regular/pipe/unix type values.
3. Parse both record sizes; reject short records, duplicate fd numbers, conflicting object metadata, and unsupported types before image emission.
4. Sanitize `O_CREAT|O_EXCL|O_TRUNC` when building `reg_file_entry`.
5. Use object IDs for file-table deduplication; never merge two records solely by path when a nonzero object ID is present.
6. Run `tests/a5-fd-contract.sh` and `tests/converter-images.sh`; commit.

### Task 3: Implement the kernel object map and safe fd snapshot

**Files:** Create `kernel_module/core/objmap.c`, `kernel_module/core/objmap.h`, modify `kernel_module/Makefile`, `kernel_module/checkpoint/dump_files.c`.

1. Implement a mutex-protected pointer-to-u32 map with `is_new`, deterministic IDs, and full cleanup.
2. Snapshot `files_struct` entries under `file_lock`, pin each file with `get_file`, release locks, then invoke the callback.
3. Classify regular files, FIFO/pipe, and sockets from inode mode/socket state without relying on unexported `pipefifo_fops`.
4. Emit extended records, clean open flags, and reject deleted paths or unsupported types before writing any record.
5. Release every file and map allocation on all error paths.
6. Run the module build in Lima against Linux 5.10.29; commit.

### Task 4: Add userspace FD fixtures and image assertions

**Files:** Create `tests/progs/fds-dup.c`, modify `tests/progs/Makefile`, create `tests/a5-converter-fds.sh`.

1. Build a fixture containing two independent opens, one dup pair, a high-numbered fd, and a pipe marker.
2. Generate an extended snapshot fixture with object IDs and sanitized truncation flags.
3. Assert `files.img` has one object entry for each object ID, `fdinfo-1.img` preserves exact fd numbers and shared IDs, and converter rejects pipe/socket records without creating an image directory.
4. Run the fixture and converter tests; commit.

### Task 5: Guest gate and regressions

**Files:** Create `tests/a5-cross-restore.sh`, modify `docs/steps/A5-fds.md`, `docs/03-Iteration-Plan.md` only for verified status.

1. Build the static fixture and module in Lima; run module load/dump only inside nested Linux 5.10.29 QEMU.
2. Verify extended records, object-id deduplication, regular file flags/position, and explicit unsupported return for pipe/socket.
3. Run A3 and A4 gates plus `make -C userspace`, shell syntax, and `git diff --check`.
4. Record fresh guest output and the exact unsupported matrix; do not add unverified ZDTM allowlist entries.
5. Commit documentation and final implementation.

## Verification commands

```sh
sh tests/a5-fd-contract.sh
sh tests/a5-converter-fds.sh
make -C userspace
sh tests/converter-format.sh
sh tests/converter-images.sh
sh -n tests/a5-*.sh
git diff --check
limactl shell criu-dev bash -lc 'cd /Users/yhome/workspace/source_code/criu_module && ./scripts/run-qemu.sh --ci --script tests/a5-cross-restore.sh'
limactl shell criu-dev bash -lc 'cd /Users/yhome/workspace/source_code/criu_module && ./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh'
```
