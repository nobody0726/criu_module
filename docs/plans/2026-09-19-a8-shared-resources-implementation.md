# A8 跨进程共享资源 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Preserve cross-process fd/fdtable/file-object and shared-memory relationships for an A7 frozen process closure, then prove restored Linux 5.10.29/aarch64 guest processes still share the same runtime objects.

**Architecture:** Add A8 object identity records to the kernel snapshot ABI, promote A5 object maps to a closure-wide dump context, make the converter consume kernel-provided `files_id`/object IDs instead of fabricating them, and emit CRIU images grouped by real shared object identity. Shared memory uses `vma_entry.shmid` plus `pagemap-shmem-$shmid.img`; the kernel still emits structured TLV records and userspace still owns protobuf/image generation.

**Tech Stack:** Linux 5.10.29/aarch64 kernel module, C11 userspace converter, CRIU protobuf wire images, A7 process-scoped TLV snapshot, Lima `criu-dev`, nested QEMU guest, static C fixtures, shell/Python contract tests, real `criu restore`.

**Spec:** `docs/plans/2026-09-19-a8-shared-resources-design.md`

---

## Global Constraints

- Do not implement restore in kernel for A8; A8 remains dump + converter + real CRIU restore validation.
- Do not make the kernel emit protobuf. Kernel output remains little-endian TLV snapshot.
- macOS only edits/Git/orchestration. Lima `criu-dev` builds. Nested Linux 5.10.29/aarch64 QEMU guest is authoritative for `insmod`, dump, converter, and restore behavior.
- Use guest local `/tmp` staging for dump, images, logs, and restore. Do not use Lima 9p paths as the restore working directory.
- Never claim success from `criu restore` exit status alone. Verify restored PID liveness, process topology, shared fd behavior, shared memory behavior, and clean dmesg.
- Preserve legacy A3-A7 snapshots unless the A8 header/record capability is present.
- Unsupported cases must return `-EOPNOTSUPP` or converter unsupported before publishing final output.
- Each implementation task should end with a small commit. Do not merge to `main` until verification-before-completion has fresh evidence.

## Plan-wide Interfaces

The implementation should use these names unless Linux 5.10.29 headers force a documented equivalent:

```c
#define CRIU_SNAPSHOT_REC_TASK_IDS 20
#define CRIU_SNAPSHOT_REC_SHMEM_OBJECT 21
#define CRIU_SNAPSHOT_REC_SHMEM_PAGE_RUN 22

struct criu_snapshot_task_ids_record {
	uint32_t version;
	uint32_t pid;
	uint32_t vm_id;
	uint32_t files_id;
	uint32_t fs_id;
	uint32_t sighand_id;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed));
```

Kernel-side closure context:

```c
struct criu_dump_shared_ctx;

int criu_dump_shared_ctx_init(struct criu_dump_shared_ctx *ctx);
void criu_dump_shared_ctx_destroy(struct criu_dump_shared_ctx *ctx);
int criu_dump_task_ids_process(struct criu_dump_shared_ctx *ctx,
			       const struct criu_freeze_process_view *view,
			       struct criu_snapshot_writer *writer);
```

Converter-side rule:

```text
ids-$pid.img.files_id == kernel TASK_IDS.files_id
fdinfo-$files_id.img  == one image per unique files_id
```

## Task 1: Lock the A8 ABI and reader contracts

**Files:**

- Modify: `include/criu_snapshot.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `userspace/criu-module-convert/snapshot_reader.h`
- Create: `tests/a8-abi-contract.sh`
- Create/Modify: `tests/fixtures/a7-snapshot-builder.py`

**Steps:**

1. Write a failing contract for `TASK_IDS`, shmem records, packed sizes, duplicate/missing task ids, shared `files_id`, and unsupported non-thread shared `vm_id`.
2. Run it in Lima and confirm it fails because A8 constants/records do not exist.
3. Add ABI definitions to `include/criu_snapshot.h`.
4. Extend `snapshot_reader.c` to parse task ids while preserving legacy A3-A7 snapshots.
5. Run `sh tests/a8-abi-contract.sh` and existing A7 converter contracts.
6. Commit with `git commit -m "test: define A8 shared resource snapshot ABI"`.

## Task 2: Add closure-wide dump shared context

**Files:**

- Create: `kernel_module/checkpoint/dump_shared.h`
- Create: `kernel_module/checkpoint/dump_shared.c`
- Modify: `kernel_module/checkpoint/Makefile`
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `kernel_module/checkpoint/dump_files.h`
- Create: `tests/a8-objmap-scope-contract.sh`

**Steps:**

1. Write a contract that fails if file object, emitted-object, or pipe objmaps are still created per process on the A7/A8 tree path.
2. Implement `criu_dump_shared_ctx` with transaction-wide maps for task ids, file objects, emitted file objects, pipe/socket emission, and future shmem ids.
3. Allocate the context after freeze settlement and free it after writer finish/abort.
4. Refactor `criu_dump_files_process()` to receive the shared context, without relaxing sharing validation yet.
5. Run `sh tests/a8-objmap-scope-contract.sh`, A5 fd contracts, and A7 dump transaction tests.
6. Commit with `git commit -m "refactor: share dump object maps across process closure"`.

## Task 3: Emit real task kobject IDs

**Files:**

- Modify: `kernel_module/checkpoint/dump_shared.c`
- Modify: `kernel_module/checkpoint/dump_shared.h`
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Create: `tests/a8-task-ids-contract.sh`

**Steps:**

1. Add converter fixtures for distinct `files_struct`, shared `files_struct`, shared non-thread `vm_id`, shared `fs_id`, and missing task ids.
2. Emit one process-scoped `CRIU_SNAPSHOT_REC_TASK_IDS` per A7 process.
3. Allocate ids by pointer identity for `mm_struct`, `files_struct`, `fs_struct`, and `sighand_struct`.
4. Parse task ids into converter process models.
5. Replace pid/constant-based `build_ids()` use with kernel-provided IDs.
6. Preserve the previous A7 fix that prevents wrong `files_id` from restoring an empty `/proc/$pid/fd`.
7. Run `sh tests/a8-task-ids-contract.sh` plus A7 converter contracts.
8. Commit with `git commit -m "feat: emit real task object ids for shared resources"`.

## Task 4: Support cross-process shared fd table and file objects

**Files:**

- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Create: `tests/progs/shared-fdt.c`
- Create: `tests/progs/shared-file-offset.c`
- Create: `tests/a8-shared-fdtable.sh`
- The shared file-offset fixture is covered by `tests/a8-shared-fdtable.sh`; the
  guest behavior gate is covered by `tests/a8-cross-restore-fd.sh`.

**Steps:**

1. Add static fixtures for inherited shared file offset and `CLONE_FILES` fdtable sharing.
2. Verify current code rejects these cases explicitly.
3. Remove the blanket cross-process `files_struct` rejection while preserving non-thread `mm_struct` rejection.
4. Validate that external `CLONE_FILES` owners are unsupported.
5. Dump one fd binding set per unique `files_id`.
6. Make converter emit exactly one `fdinfo-$files_id.img` per unique fd table.
7. Add image-name assertions so `fdinfo-$pid.img` is not accidentally used on the A8 path.
8. Run converter-only A8 fd tests plus A5/A7 regressions.
9. Commit with `git commit -m "feat: preserve shared fd tables across processes"`.

## Task 5: Extend cross-process pipe and UNIX socket closure validation

**Files:**

- Modify: `kernel_module/checkpoint/dump_files.c`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Create: `tests/progs/a8-cross-fd.c`
- Create: `tests/a8-cross-ipc.sh`

**Steps:**

1. Add a parent/child pipe fixture with unread bytes.
2. Add a parent/child `socketpair(AF_UNIX, SOCK_STREAM)` fixture with queued bytes.
3. Ensure pipe endpoint, pipe data, UNIX socket record, and socket queue emission use closure-wide maps.
4. Keep rejecting ancillary data, external peers, pathname-bound sockets, datagram/seqpacket, packetized pipe, and unknown pipe buffer types.
5. Run `sh tests/a8-cross-ipc.sh --converter-only` and A5 fd contracts.
6. Commit with `git commit -m "feat: deduplicate cross-process pipe and unix socket objects"`.

## Task 6: Run A8.1 real guest cross-restore gate

**Files:**

- Create: `tests/a8-cross-restore-fd.sh`
- Modify: `scripts/run-qemu.sh` only if staging lacks needed fixture support
- Modify: `docs/steps/A8-shared-resources.md`

**Steps:**

1. Build a QEMU guest script that compiles fixtures in Lima, stages them to guest local `/tmp`, loads the module, dumps, converts, restores with real CRIU, and verifies behavior markers.
2. Verify shared file offset after restore.
3. Verify `CLONE_FILES` fdtable sharing after restore.
4. Verify cross-process pipe/socket communication after restore.
5. Fail on restored PID death, missing behavior markers, OOPS/WARN/refcount splat, dirty dmesg, or unload failure.
6. Run:

   ```sh
   limactl shell criu-dev bash -lc \
     'cd /Users/yhome/workspace/source_code/criu_module && \
      ./scripts/run-qemu.sh --ci --script tests/a8-cross-restore-fd.sh'
   ```

7. Expected: `A8_FD_CROSS_RESTORE: PASS`.
8. Commit with `git commit -m "test: verify A8 shared fd restore in guest"`.

## Task 7: Add shared-memory snapshot records and converter model

**Files:**

- Modify: `include/criu_snapshot.h`
- Modify: `kernel_module/checkpoint/dump_mm.c`
- Modify: `kernel_module/checkpoint/dump_mm.h`
- Create: `kernel_module/checkpoint/dump_shmem.c`
- Create: `kernel_module/checkpoint/dump_shmem.h`
- Modify: `kernel_module/checkpoint/Makefile`
- Modify: `userspace/criu-module-convert/criu_model.c`
- Create: `tests/a8-shmem-contract.sh`

**Steps:**

1. Write a failing contract with two process-scoped VMAs referencing one `shmid`, different virtual addresses, and one object content stream.
2. Classify supported shmem-backed VMAs using A1/A8 documented rules; do not depend on unavailable `vma_is_shmem()` exports.
3. Allocate `shmid` by shmem inode identity.
4. Dump shmem content once from `inode->i_mapping`; skip holes and do not fault absent pages.
5. Generate `vma_entry.shmid` and `pagemap-shmem-$shmid.img`.
6. Keep ordinary file-backed `MAP_SHARED` content out of pages and document the semantic gap.
7. Run shmem contract and A3 converter regression.
8. Commit with `git commit -m "feat: model shared memory objects in A8 snapshots"`.

## Task 8: Add shared-memory guest behavior gates

**Files:**

- Create: `tests/progs/shm-anon.c`
- Create: `tests/progs/shm-posix.c`
- Create: `tests/a8-cross-restore-shmem.sh`
- Modify: `docs/steps/A8-shared-resources.md`

**Steps:**

1. Add anonymous shared-memory parent/child fixture with bidirectional sequence and fixed pattern checks.
2. Add POSIX shm or memfd/tmpfs fixture compatible with the Linux 5.10.29 guest.
3. Add image-size assertion that shared payload is approximately one copy, not one copy per process.
4. Run:

   ```sh
   limactl shell criu-dev bash -lc \
     'cd /Users/yhome/workspace/source_code/criu_module && \
      ./scripts/run-qemu.sh --ci --script tests/a8-cross-restore-shmem.sh'
   ```

5. Expected: `A8_SHMEM_CROSS_RESTORE: PASS`.
6. Commit with `git commit -m "test: verify A8 shared memory restore in guest"`.

## Task 9: SysV shm feasibility branch

**Files:**

- Create: `tests/progs/shm-sysv.c`
- Create: `tests/a8-sysv-shm-feasibility.sh`
- Modify: `docs/plans/2026-09-19-a8-shared-resources-design.md`
- Modify: `docs/steps/A8-shared-resources.md`

**Steps:**

1. Add a small SysV shm parent/child sharing fixture.
2. Probe whether the module can safely identify SysV shm VMAs and associate them with stable object IDs on Linux 5.10.29.
3. If feasible, implement using the same `shmid`/single-content model.
4. If not feasible, add explicit `-EOPNOTSUPP` detection and document why.
5. Run `sh tests/a8-sysv-shm-feasibility.sh`.
6. Expected: either `A8_SYSV_SHM: PASS` or `A8_SYSV_SHM: UNSUPPORTED_RECORDED`.
7. Commit with `git commit -m "docs: record A8 sysv shm feasibility result"`.

## Task 10: Regression, documentation, and release gate

**Files:**

- Modify: `docs/steps/A8-shared-resources.md`
- Modify: `docs/03-Iteration-Plan.md` only if A8 status wording changes are needed
- Modify: `docs/plans/2026-09-18-a5-a6-extension-backlog.md` only for newly deferred A8 extension items
- Create: `docs/A8-问题与解决方法复盘.md`
- Create: `tests/a8-smoke.sh`

**Steps:**

1. Add A8 smoke script for syntax/contracts/converter checks and guest gates.
2. Run `sh tests/a8-smoke.sh` in Lima.
3. Run A3-A7 core regressions documented in their implementation plans.
4. Write A8 retrospective with object identity bugs, CRIU image naming constraints, shmem path correction, environment issues, and unsupported follow-ups.
5. Run `git status --short` and `git diff --check`; ensure no generated binaries, logs, image dirs, or `artifacts/`.
6. Commit with `git commit -m "docs: finalize A8 shared resource validation"`.

## Completion Checklist

- [ ] A8 ABI contract passes.
- [ ] Closure-wide objmap contract passes.
- [ ] Converter emits `ids-$pid.img` from kernel task ids.
- [ ] Converter emits one `fdinfo-$files_id.img` per unique fdtable.
- [ ] Shared file offset guest restore passes.
- [ ] `CLONE_FILES` fdtable guest restore passes.
- [ ] Cross-process pipe/socket guest restore passes.
- [ ] Anonymous shared-memory guest restore passes.
- [ ] POSIX shm/memfd shared-memory guest restore passes or documented equivalent passes.
- [ ] SysV shm is either supported or explicitly unsupported with feasibility evidence.
- [ ] A3-A7 core regressions still pass.
- [ ] Guest dmesg is clean after A8 gates.
- [ ] Documentation records unsupported and post-A9 extension items.
