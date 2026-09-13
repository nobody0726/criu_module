# A3 Minimal Dump Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a kernel-collected `snapshot.bin`, a user-space CRIU image converter, and the real `criu restore` A3 gate for the defined minimal process.

**Architecture:** The kernel module freezes the target through A2, collects normalized task/mm/VMA/page/file state into one versioned TLV snapshot, validates consistency, and atomically commits it. A user-space converter validates the snapshot and emits the repository CRIU protobuf image set; CRIU remains responsible for restore.

**Tech Stack:** Linux 5.10.29 out-of-tree module, C, kernel file/page APIs, CRIU protobuf-c definitions, `crit`, shell integration tests, and a two-level Lima+QEMU validation environment. Lima `criu-dev` is the build/orchestration guest; the nested disposable QEMU guest booted by `scripts/run-qemu.sh` is the only environment allowed to load the module or run kernel tests.

---

### Task 1: Establish the snapshot ABI and fixtures

**Files:**
- Create: `include/criu_snapshot.h`
- Create: `tests/snapshot-format.sh`
- Create: `tests/fixtures/snapshot-minimal.bin` through a small fixture generator

**Step 1: Write the failing test**

Define tests for magic, version, little-endian fixed-width fields, TLV headers,
record limits, `END`, footer checksum, and rejection of truncated records.

**Step 2: Run test to verify it fails**

Run: `bash tests/snapshot-format.sh`

Expected: FAIL because the ABI reader/generator does not exist.

**Step 3: Write minimal implementation**

Define explicit packed-on-disk field widths, record IDs, flags, status codes,
maximum record/payload sizes, and checksum coverage. Keep the header independent
of kernel-private structs and document endianness/alignment.

**Step 4: Run test to verify it passes**

Run: `bash tests/snapshot-format.sh`

Expected: `SNAPSHOT_FORMAT: PASS`.

**Step 5: Commit**

```bash
git add include/criu_snapshot.h tests/snapshot-format.sh tests/fixtures
git commit -m "test: define A3 snapshot ABI"
```

### Task 2: Add kernel snapshot writer and atomic lifecycle

**Files:**
- Create: `kernel_module/checkpoint/snapshot_writer.c`
- Create: `kernel_module/checkpoint/snapshot_writer.h`
- Modify: `kernel_module/Makefile`
- Test: `tests/snapshot-writer.sh`

**Step 1: Write the failing test**

Exercise temporary-file creation, bounded record writes, checksum finalization,
atomic rename, and cleanup after injected write failure.

**Step 2: Run test to verify it fails**

Run: `bash tests/snapshot-writer.sh`

Expected: FAIL because writer entry points are absent.

**Step 3: Write minimal implementation**

Implement sequential writes using the kernel file API, overflow-safe length
checks, checksum accumulation, `END`/footer emission, flush, and rename. Never
publish the final path before validation completes.

**Step 4: Run test to verify it passes**

Run: `bash tests/snapshot-writer.sh`

Expected: `SNAPSHOT_WRITER: PASS`.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/snapshot_writer.* kernel_module/Makefile tests/snapshot-writer.sh
git commit -m "feat: add atomic kernel snapshot writer"
```

### Task 3: Implement task, register, and filesystem collection

**Files:**
- Create: `kernel_module/checkpoint/dump_task.c`
- Create: `kernel_module/checkpoint/dump_task.h`
- Create: `kernel_module/checkpoint/dump_files.c`
- Create: `kernel_module/checkpoint/dump_files.h`
- Test: `tests/dump-task-contract.sh`

**Step 1: Write the failing test**

Assert PID/TGID, target identity, freeze generation, register record presence,
signal mask/rlimit fields, fd 0/1/2 regular-file metadata, cwd/root, and explicit
rejection of extra or non-regular descriptors.

**Step 2: Run test to verify it fails**

Run: `bash tests/dump-task-contract.sh`

Expected: FAIL because collection records are absent.

**Step 3: Write minimal implementation**

Collect only the A3 scope. Use pinned task/files references, copy stable scalar
metadata while frozen, and report `UNSUPPORTED` for extra descriptors, sockets,
handlers, pending signals, timers, or unsupported filesystem state.

**Step 4: Run test to verify it passes**

Run: `bash tests/dump-task-contract.sh`

Expected: `DUMP_TASK: PASS`.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/dump_task.* kernel_module/checkpoint/dump_files.* tests/dump-task-contract.sh
git commit -m "feat: collect A3 task and file state"
```

### Task 4: Implement normalized VMA collection and policy rejection

**Files:**
- Create: `kernel_module/checkpoint/dump_mm.c`
- Create: `kernel_module/checkpoint/dump_mm.h`
- Modify: `kernel_module/checkpoint/vma_walk.c` if needed
- Test: `tests/dump-vma-policy.sh`

**Step 1: Write the failing test**

Cover ordinary private anonymous/file VMAs, vDSO, vvar, guard/`PROT_NONE`, and
rejection of shared mappings, hugetlb, IO/PFNMAP/MIXEDMAP, and unknown classes.

**Step 2: Run test to verify it fails**

Run: `bash tests/dump-vma-policy.sh`

Expected: FAIL because A3 policy classification is not implemented.

**Step 3: Write minimal implementation**

Reuse A1 VMA traversal and normalized classification. Store semantic protection,
mapping class, special type, dump policy, backing identity/path, offset, and the
per-VMA page counters. Do not persist raw `vm_flags` as the restore ABI.

**Step 4: Run test to verify it passes**

Run: `bash tests/dump-vma-policy.sh`

Expected: `DUMP_VMA_POLICY: PASS`.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/dump_mm.* kernel_module/checkpoint/vma_walk.c tests/dump-vma-policy.sh
git commit -m "feat: classify A3 VMAs and enforce policy"
```

### Task 5: Implement non-faulting page classification and PAGE_RUN capture

**Files:**
- Modify: `kernel_module/checkpoint/dump_mm.c`
- Create: `kernel_module/checkpoint/page_scan.c`
- Create: `kernel_module/checkpoint/page_scan.h`
- Test: `tests/page-policy.sh`

**Step 1: Write the failing test**

Verify absent pages do not become resident, zero pages are skipped, file-backed
private pages are skipped until COW, COW pages are saved, vDSO is fully saved,
vvar/guard pages are not read, and swapped pages return `UNSUPPORTED`.

**Step 2: Run test to verify it fails**

Run: `bash tests/page-policy.sh`

Expected: FAIL because page classification/copy is absent.

**Step 3: Write minimal implementation**

Walk page tables or the Linux 5.10.29 equivalent without faulting absent pages.
Classify present/zero/file-backed/swap/guard states first, acquire page
references for selected pages, copy payloads, coalesce adjacent pages into
`PAGE_RUN`, and update counters. Reject swap and unsupported special pages.

**Step 4: Run test to verify it passes**

Run: `bash tests/page-policy.sh`

Expected: `PAGE_POLICY: PASS`.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/dump_mm.c kernel_module/checkpoint/page_scan.* tests/page-policy.sh
git commit -m "feat: capture A3 pages without faulting"
```

### Task 6: Implement dump orchestration and consistency checks

**Files:**
- Create: `kernel_module/checkpoint/dump.c`
- Create: `kernel_module/checkpoint/dump.h`
- Modify: `kernel_module/core/main.c`
- Test: `tests/dump-errors.sh`

**Step 1: Write the failing test**

Exercise `dump <pid> <snapshot-path>`, target exit, generation mismatch, mm/VMA
change detection, unsupported resource rejection, I/O failure, and unconditional
thaw.

**Step 2: Run test to verify it fails**

Run: `bash tests/dump-errors.sh`

Expected: FAIL because the dump control path is absent.

**Step 3: Write minimal implementation**

Implement `freeze -> collect -> revalidate -> commit -> thaw`, compare pinned task,
PID/TGID, generation, mm pointer, VMA count and normalized VMA tuples, and remove
the temporary snapshot on every failed path. Expose stable error classes.

**Step 4: Run test to verify it passes**

Run: `bash tests/dump-errors.sh`

Expected: `DUMP_ERRORS: PASS` and no residual frozen target.

**Step 5: Commit**

```bash
git add kernel_module/checkpoint/dump.* kernel_module/core/main.c tests/dump-errors.sh
git commit -m "feat: orchestrate A3 dump with consistency gates"
```

### Task 7: Build the user-space snapshot reader and validator

**Files:**
- Create: `userspace/criu-module-convert/snapshot_reader.c`
- Create: `userspace/criu-module-convert/snapshot_reader.h`
- Create: `userspace/criu-module-convert/main.c`
- Create: `userspace/criu-module-convert/Makefile`
- Test: `tests/converter-format.sh`

**Step 1: Write the failing test**

Feed valid, truncated, checksum-corrupt, schema-mismatched, overflowing, and
unknown-mandatory-record snapshots to the converter.

**Step 2: Run test to verify it fails**

Run: `bash tests/converter-format.sh`

Expected: FAIL because the converter does not exist.

**Step 3: Write minimal implementation**

Read the complete file before output, validate all bounds and cross-record counts,
verify checksum/footer, and return `FORMAT_ERROR`, `UNSUPPORTED`, or `IO_ERROR`.

**Step 4: Run test to verify it passes**

Run: `bash tests/converter-format.sh`

Expected: `CONVERTER_FORMAT: PASS`.

**Step 5: Commit**

```bash
git add userspace/criu-module-convert tests/converter-format.sh
git commit -m "feat: validate A3 snapshots in user space"
```

### Task 8: Emit CRIU protobuf images

**Files:**
- Create: `userspace/criu-module-convert/criu_model.c`
- Create: `userspace/criu-module-convert/criu_model.h`
- Create: `userspace/criu-module-convert/image_writer.c`
- Create: `userspace/criu-module-convert/image_writer.h`
- Modify: `userspace/criu-module-convert/Makefile`
- Test: `tests/converter-images.sh`

**Step 1: Write the failing test**

Assert all A3 image names, CRIU magic, protobuf fields, IDs, VMA attributes,
`pagemap_head`, page offsets in page units, and file/fd cross references.

**Step 2: Run test to verify it fails**

Run: `bash tests/converter-images.sh`

Expected: FAIL because image emission is absent.

**Step 3: Write minimal implementation**

Reuse the reference CRIU `.proto` generated code. Map normalized snapshot records
to `inventory`, `pstree`, `core`, `mm`, `pagemap`, `pages`, `files`, `fdinfo`,
`reg-files`, `ids`, `fs`, and `creds`. Stage each output and publish only after
all files succeed.

**Step 4: Run test to verify it passes**

Run: `bash tests/converter-images.sh`

Expected: `CONVERTER_IMAGES: PASS`.

**Step 5: Commit**

```bash
git add userspace/criu-module-convert tests/converter-images.sh
git commit -m "feat: convert A3 snapshots to CRIU images"
```

### Task 9: Add reference comparison and real restore gate

**Files:**
- Create: `tests/progs/minimal.c`
- Create: `tests/criu-field-compare.sh`
- Create: `tests/cross-restore.sh`
- Modify: `tests/ci-smoke.sh`
- Modify: `docs/steps/A3-minimal-dump.md`

**Step 1: Write the failing test**

Run a real CRIU dump and a module dump for the same minimal program, decode both
with `crit`, compare fields using an explicit difference allowlist, then restore
the module-produced images and validate program state.

**Step 2: Run test to verify it fails**

Run: `bash tests/cross-restore.sh`

Expected: FAIL until the module dump and converter are integrated.

**Step 3: Write minimal implementation**

Build a static single-thread test process with regular-file fds and deterministic
memory markers. Add comparison diagnostics and the final `criu restore` command.

**Step 4: Run test to verify it passes**

Run from the macOS host by dispatching the command into the Lima `criu-dev` build guest; the command then starts the disposable Linux 5.10.29 QEMU guest. Do not run `cross-restore.sh` directly in Lima, because Lima's kernel is the build/orchestration kernel (normally 5.15), not the target 5.10.29 test kernel:

```bash
limactl shell criu-dev bash -lc 'cd /Users/yhome/workspace/source_code/criu_module && ./scripts/run-qemu.sh --ci --script tests/cross-restore.sh'
```

Expected: `A3_CROSS_RESTORE: PASS`. The 5.10.29 `Image` and initramfs must exist inside the Lima guest under `$HOME/kernels` (for this guest, `/home/yhome.guest/kernels`).

**Restore-path invariant:** The target starts with cwd/root `/`, so the guest
initramfs root must be mode `0755`. Keep all generated snapshot and CRIU images
under guest-local `/tmp`, never `/mnt/host`; invoke restore with `(cd / && ...)`
and absolute image paths. This preserves CRIU's default file-mode validation
without `--skip-file-rwx-check`. The snapshot stores `vm_pgoff` in pages, while
the emitted CRIU `vma_entry.pgoff` must be converted to bytes using the snapshot
page size.

**Step 5: Commit**

```bash
git add tests/progs/minimal.c tests/criu-field-compare.sh tests/cross-restore.sh tests/ci-smoke.sh docs/steps/A3-minimal-dump.md
git commit -m "test: add A3 CRIU comparison and restore gate"
```

### Task 10: Final verification and documentation integration

**Files:**
- Modify: `docs/03-Iteration-Plan.md`
- Modify: `docs/steps/A3-minimal-dump.md`
- Test: `tests/ci-smoke.sh`, `tests/module-smoke.sh`

**Step 1: Run the complete validation suite**

```bash
git diff --check
for f in tests/*.sh tests/compare/*.sh; do bash -n "$f"; done
limactl shell criu-dev bash -lc 'make -C /home/yhome.guest/kernels/verify-a3-kernel M=/Users/yhome/workspace/source_code/criu_module/kernel_module modules'
limactl shell criu-dev bash -lc 'cd /Users/yhome/workspace/source_code/criu_module && ./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh'
```

Expected: build succeeds, shell checks pass, all A3 contract gates emit PASS,
and the final line is `CI_RESULT: PASS`.

**Step 2: Update status documents**

Link this design and implementation plan from the A3 step document. Mark A3
complete only after real restore succeeds; record any environment-only warnings
separately from code failures.

**Step 3: Commit**

```bash
git add docs/03-Iteration-Plan.md docs/steps/A3-minimal-dump.md tests/ci-smoke.sh tests/module-smoke.sh
git commit -m "docs: integrate A3 implementation gates"
```
