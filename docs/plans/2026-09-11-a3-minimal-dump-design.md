# A3 Minimal Dump Design

**Date:** 2026-09-11  
**Status:** Approved design  
**Scope:** A3 minimal single-process dump; no implementation in this document

## 1. Goal and boundary

A3 is the A-track gate. It must prove that a snapshot collected by the kernel
module can be converted into images accepted by the repository's reference CRIU
and restored by a real `criu restore`.

The target is intentionally narrow:

- one process and one thread;
- statically linked executable;
- file descriptors 0, 1, and 2 only, each referring to a regular file;
- no child process, socket, shared-memory object, signal handler, pending signal,
  or timer;
- cwd and root are `/`;
- the target is synchronously frozen by A2 for the whole collection phase.

A3 does not implement restore. User space CRIU remains the restore engine.

## 2. Architecture

The data flow is:

```text
dump control
  -> A2 freeze
  -> kernel state collection
  -> snapshot.bin
  -> criu-module-convert
  -> CRIU image set
  -> real criu restore
```

The kernel module produces only a versioned intermediate snapshot. The user-space
converter owns CRIU protobuf encoding and image naming.

### Kernel module responsibilities

- `snapshot_writer`: temporary file, sequential records, bounds, checksum, and
  atomic commit;
- `dump_task`: identity, registers, signal mask, rlimits, credentials, and IDs;
- `dump_mm`: mm summary, normalized VMA attributes, page classification, and
  `PAGE_RUN` payloads;
- `dump_files`: regular-file metadata for fd 0/1/2 and filesystem context;
- `dump`: orchestration, consistency checks, and unconditional thaw on every
  exit path.

### User-space converter responsibilities

- `snapshot_reader`: header/TLV/footer validation and checksum verification;
- `criu_model`: mapping normalized records to the reference CRIU protobuf model;
- `image_writer`: atomic creation of the CRIU image set and cross-reference IDs.

## 3. Intermediate snapshot format

The input is one self-contained file, `snapshot.bin`. Page payloads are embedded
in the same file; A3 does not create a second page-data input.

```text
header
TLV records
  TASK, MM, VMA, REGS, FD, FS, CREDS, IDMAP, PAGE_RUN, END
index/footer
```

The header contains at least:

```text
magic
format_version
producer_version
criu_schema_version
kernel_release
arch
page_size
target_pid
target_tgid
freeze_generation
record_count
total_size
checksum
```

Each record has a type, flags, length, sequence number, and bounded payload. A
valid snapshot must contain an `END` record and a footer checksum. The converter
rejects truncation, integer overflow, unknown mandatory records, and schema
version mismatch.

`PAGE_RUN` contains the virtual start address, page count, page size, page flags,
payload length, and inline page bytes. It may also represent a zero run without
payload. Per-VMA counters record pages present, saved, skipped as zero, skipped
as file-backed, and skipped for other policy reasons. These counters are
diagnostic and do not alter restore semantics.

## 4. VMA and page policy

VMA attributes are normalized semantic fields, not a persisted raw `vm_flags`
integer. The collector records protection, private/shared mapping semantics,
anonymous/file backing, special type, dump policy, lock state, file identity,
path, offset, and length.

The page algorithm is **classify first, read second**. It must inspect page-table
state without faulting in an absent page, then copy only pages known to require
content preservation.

| Region/page state | A3 policy |
|---|---|
| private anonymous, resident and non-zero | save in `PAGE_RUN` |
| private anonymous, absent or zero page | skip or zero-run |
| private file, still file-backed | skip; restore from file |
| private file, already COWed | save in `PAGE_RUN` |
| vDSO | save the complete VMA |
| vvar | preserve VMA metadata; do not save page bytes |
| guard or `PROT_NONE` | preserve VMA metadata; do not read bytes |
| swapped page requiring preservation | reject with `UNSUPPORTED` |
| shared anonymous/file mapping | reject in A3 |
| hugetlb, DAX, IO, PFNMAP, MIXEDMAP | reject in A3 |

This follows the reference CRIU paths in `criu/criu/mem.c` and
`criu/criu/proc_parse.c`: private mappings and anonymous shared mappings are
the generic page-dump candidates, vDSO is dumped fully, vvar is skipped, file
private pages that remain file-backed are omitted, and unsupported mappings are
rejected rather than silently serialized.

The A3 policy for unsupported resources is whole-dump rejection. No partial
snapshot is exposed to the converter.

## 5. Consistency and atomicity

The dump sequence is:

1. create `snapshot.bin.tmp`;
2. capture the target task reference and A2 freeze generation;
3. collect task, mm, VMA, register, file, and page records;
4. retain page references while copying selected pages;
5. re-check task identity, PID/TGID, generation, mm pointer, VMA count, and each
   VMA's start/end/protection/class;
6. append `END`, index/footer, and checksum;
7. flush the temporary file and atomically rename it to `snapshot.bin`;
8. thaw the target, including on all error paths.

Exit, mm destruction, generation changes, VMA changes, page-state failures, and
I/O failures prevent commit. The temporary file is removed on failure. A thaw
failure is reported separately and never turns an unsuccessful dump into a
successful one.

## 6. Converter and CRIU images

The converter is an independent command:

```bash
criu-module-convert snapshot.bin -D /tmp/a3-images
```

It reuses the same CRIU `.proto` definitions as the reference tree. It first
validates the complete snapshot, then writes temporary image files and atomically
publishes the output directory entries. It must generate the A3 image set:

```text
inventory.img
pstree.img
core-$pid.img
mm-$pid.img
pages-1.img
pagemap-$pid.img
files.img
fdinfo-$pid.img
reg-files.img
ids-$pid.img
fs-$pid.img
creds-$pid.img
```

The first pagemap record is `pagemap_head` with `pages_id`; subsequent offsets
are measured in pages. Only present page payloads are written to `pages-1.img`.

Converter errors are classified as `FORMAT_ERROR`, `UNSUPPORTED`, `IO_ERROR`,
or `CRIU_REJECTED`.

File-backed records include path, device, inode, size, mtime/ctime, mapping
offset, length, flags, and permissions. A3 does not package file contents or
provide relocation. Renaming a file may work if identity and content remain
available; deletion, replacement, size changes, or inode mismatch may make
restore fail, as with the reference CRIU semantics.

## 7. Verification gates

The acceptance pipeline has four layers:

1. snapshot contract tests: records, bounds, checksum, counters, freeze/thaw,
   unsupported-resource rejection, and cleanup;
2. converter fixtures: protobuf fields, image names, pagemap/pages layout,
   malformed-input rejection, and no partial output;
3. field comparison against a real CRIU dump decoded with `crit`, with only
   explicitly documented differences allowed;
4. end-to-end restore:

   ```bash
   criu-module-dump "$PID" /tmp/a3/snapshot.bin
   criu-module-convert /tmp/a3/snapshot.bin -D /tmp/a3/images
   criu restore -D /tmp/a3/images --shell-job
   ```

A3 passes only when all four layers pass, the restored program validates its
  expected state, the target is no longer frozen, and dmesg contains no warning,
  OOPS, lockdep, or KASAN report.

## 8. Reference sources

The design is grounded in the existing project documents, especially
`docs/principles/03-memory-and-vma.md`,
`docs/principles/10-vma-semantics-and-attributes.md`,
`docs/steps/A3-minimal-dump.md`, and the A1/A2 design documents. The CRIU
behavior was checked against `criu/criu/mem.c`, `criu/criu/include/vma.h`,
`criu/criu/proc_parse.c`, and `criu/criu/pagemap-cache.c`.
