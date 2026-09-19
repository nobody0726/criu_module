# A6 Signals and Timers Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend the Linux 5.10.29 kernel dump module and userspace converter to preserve signal dispositions, pending queues, interval timers, and POSIX timer state in CRIU-compatible core images, with real guest cross-restore evidence.

**Architecture:** The kernel collects structured A6 records under the A2 freeze context and publishes them through the existing little-endian snapshot TLV ABI. The userspace converter performs complete model validation before generating CRIU core protobuf fields in an atomic staging directory. The first gate captures and serializes POSIX timer overrun, but does not claim exact post-restore `timer_getoverrun()` behavior because stock CRIU does not apply the field during restore.

**Tech Stack:** Linux 5.10.29/aarch64 kernel module, C, existing snapshot TLV ABI, protobuf wire writer, CRIU `core.proto`, Lima + nested QEMU guest, shell/Python contract fixtures, real CRIU restore.

---

## Preconditions and global rules

- Work from a clean feature branch or dedicated worktree; do not modify `main` directly.
- Read [A6 design](2026-09-18-a6-signals-timers-design.md) and [A3 retrospective](../A3-问题与解决方法复盘.md) before every implementation wave.
- Use Linux 5.10.29 sources in the Lima `criu-dev` environment for kernel field and lock verification.
- Never perform file I/O, `GFP_KERNEL` allocation, or sleeping operations while holding `sighand->siglock` or a timer `it_lock`.
- Do not emit protobuf from the kernel.
- A snapshot with `CRIU_SNAPSHOT_F_SIGNAL_TIMERS` must contain every required A6 record. Do not silently fall back to default sigactions, empty queues, or zero timers.
- Every failure must abort the temporary snapshot/image output and leave no partial result.
- A real restore gate requires post-restore behavior and process-liveness checks; CRIU exit status alone is insufficient.

## Task 1: Lock the A6 ABI contract and host-side parser fixtures

**Files:**
- Modify: `include/criu_snapshot.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `userspace/criu-module-convert/snapshot_reader.h`
- Create: `tests/a6-abi-contract.sh`
- Create: `tests/fixtures/a6-snapshot-builder.py`
- Test: `tests/snapshot-format.sh`, `tests/converter-format.sh`

### Step 1: Add failing ABI assertions and fixture cases

Extend the Python fixture builder with header capability flags and record types 15–18. Add cases for missing records, duplicate records, bad queue chunk ranges, invalid siginfo length, and unknown mandatory records.

Run:

```bash
./tests/a6-abi-contract.sh
```

Expected: FAIL because the header flag and record validators do not yet exist.

### Step 2: Define the wire constants and packed-size assertions

Add:

- `CRIU_SNAPSHOT_F_SIGNAL_TIMERS`;
- `CRIU_SNAPSHOT_REC_SIGACTION` through `CRIU_SNAPSHOT_REC_POSIX_TIMERS`;
- record version constants;
- fixed siginfo size `128`;
- scope and timer-kind constants;
- explicit little-endian wire structs or offset constants for headers and entries.

Do not use kernel `struct k_sigaction`, `struct sigqueue`, or `struct k_itimer` as serialized structs.

### Step 3: Make the reader validate A6 capability semantics

The reader must accept legacy header flags `0` and A6 flags, reject unknown mandatory header flags, and preserve the existing rule that unknown mandatory record types are unsupported rather than ignored.

### Step 4: Run the ABI tests

```bash
make -C userspace/criu-module-convert clean all
./tests/snapshot-format.sh
./tests/converter-format.sh
./tests/a6-abi-contract.sh
```

Expected: PASS for legacy fixtures and PASS for valid A6 fixtures; negative fixtures must fail with format/unsupported status and no output directory.

### Step 5: Commit

```bash
git add include/criu_snapshot.h userspace/criu-module-convert/snapshot_reader.* tests/a6-abi-contract.sh tests/fixtures/a6-snapshot-builder.py
git commit -m "test: define A6 signal timer snapshot ABI"
```

## Task 2: Expose the frozen task set and add A6 collection interfaces

**Files:**
- Modify: `kernel_module/checkpoint/freeze.c`
- Modify: `kernel_module/checkpoint/criu_freezer.h`
- Modify: `kernel_module/checkpoint/dump.c`
- Create: `kernel_module/checkpoint/dump_signals.h`
- Create: `kernel_module/checkpoint/dump_timers.h`
- Modify: `kernel_module/Makefile` or the module source list
- Test: `tests/dump-task-contract.sh`, new lock-boundary contract checks

### Step 1: Add contract tests for freeze-context ownership

Check that A6 receives the A2-pinned task set and does not independently treat a fresh thread enumeration as authoritative.

Run:

```bash
./tests/dump-task-contract.sh
```

Expected: FAIL until the new accessor/signature exists.

### Step 2: Add a read-only frozen-task accessor

Expose only the minimum information needed by dump collectors:

- task pointer with an existing reference;
- captured TID;
- task count;
- target generation.

Do not expose mutable freezer internals or transfer ownership accidentally. Document that callers must not sleep while using the returned lock-protected view.

### Step 3: Define collector-owned snapshot structures

Create internal structures for:

- 64 sigaction entries;
- shared/private queue chunks and synthetic siginfo entries;
- interval timer entries;
- POSIX timer entries;
- capture fingerprints and retry state.

These structures are memory-only and must not be written directly as ABI payloads.

### Step 4: Run build and static checks

```bash
make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29
./tests/dump-task-contract.sh
```

Expected: module build succeeds and the contract test confirms the freeze context remains the identity source.

### Step 5: Commit

```bash
git add kernel_module/checkpoint/freeze.c kernel_module/checkpoint/criu_freezer.h kernel_module/checkpoint/dump.c kernel_module/checkpoint/dump_signals.h kernel_module/checkpoint/dump_timers.h kernel_module/Makefile tests/dump-task-contract.sh
git commit -m "feat: expose frozen task set for A6 collectors"
```

## Task 3: Implement sigaction and pending-queue capture

**Files:**
- Create: `kernel_module/checkpoint/dump_signals.c`
- Modify: `kernel_module/checkpoint/dump_signals.h`
- Modify: `kernel_module/checkpoint/dump_task.c`
- Modify: `kernel_module/checkpoint/dump_threads.c`
- Create: `tests/progs/sig-handlers.c`
- Create: `tests/progs/sig-pending.c`
- Modify: `tests/progs/Makefile`

### Step 1: Add failing kernel contract checks

Assert in source tests that signal actions and queue traversal occur under `siglock`, while snapshot writer calls occur after the lock is released. Add checks rejecting `kernel_write` or `GFP_KERNEL` allocation in the critical section.

Run:

```bash
./tests/a6-abi-contract.sh --kernel-contract
```

Expected: FAIL until the collector and lock annotations are present.

### Step 2: Implement preflight counting

Under `siglock`, count:

- action entries;
- each queue's list entries;
- bit-only pending signals that require `collect_signal()`-compatible synthetic info;
- total queue payload size.

Reject unrepresentable sizes with a clear bounded error. Do not allocate under the lock.

### Step 3: Implement the locked copy pass

Copy:

- `sighand->action[0.._NSIG-1]` into explicit fields;
- queue masks;
- raw `kernel_siginfo_t` bytes from each `sigqueue` node;
- synthetic `SI_USER` records for bit-only pending signals;
- owner scope and TID.

Validate that raw `si_signo` matches the queue entry signal. Use a compile-time or build-time assertion that the native aarch64 siginfo payload is 128 bytes.

### Step 4: Emit records only after unlocking

Generate one or more `SIGACTION` and `SIGNAL_QUEUE` payloads from memory. Queue chunks must carry `total_count`, `first_index`, and `entry_count` so the converter can detect gaps and overlaps.

### Step 5: Add fixtures

`sig-handlers.c` must install distinct handlers, masks, flags, ignored/default actions, and a restorer-compatible native disposition. `sig-pending.c` must exercise shared signals, `tgkill`, realtime queue order, and a bit-only legacy signal.

### Step 6: Build and run host/guest-independent checks

```bash
make -C tests/progs clean all
make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29
./tests/a6-abi-contract.sh
```

Expected: valid record fixtures pass; malformed and lock-boundary cases fail explicitly.

### Step 7: Commit

```bash
git add kernel_module/checkpoint/dump_signals.* kernel_module/checkpoint/dump_task.c kernel_module/checkpoint/dump_threads.c tests/progs/sig-handlers.c tests/progs/sig-pending.c tests/progs/Makefile tests/a6-abi-contract.sh
git commit -m "feat: collect signal actions and pending queues"
```

## Task 4: Implement timer capture with Linux 5.10.29 lock ordering

**Files:**
- Create: `kernel_module/checkpoint/dump_timers.c`
- Modify: `kernel_module/checkpoint/dump_timers.h`
- Modify: `kernel_module/checkpoint/dump_task.c`
- Create: `tests/progs/timers.c`
- Modify: `tests/progs/Makefile`
- Test: `tests/a6-abi-contract.sh`

### Step 1: Add failing timer lock-order tests

Verify that source and review checks enforce:

```text
timer->it_lock -> sighand->siglock
```

and reject an implementation that acquires `it_lock` while holding `siglock`.

Run:

```bash
./tests/a6-abi-contract.sh --timer-locks
```

Expected: FAIL before the collector is implemented.

### Step 2: Implement interval timer capture

Under `siglock`, capture REAL, VIRTUAL, and PROF using the Linux 5.10.29 semantics from `kernel/time/itimer.c`:

- REAL uses hrtimer remaining time and `it_real_incr`;
- VIRTUAL/PROF use `signal->it[]` and a frozen thread-group CPU sample;
- active-but-expired timers use a minimal non-zero value;
- disarmed timers use zero.

### Step 3: Implement POSIX timer pointer collection

Under `siglock` + RCU, count and collect `signal->posix_timers` pointers. Release `siglock` while retaining RCU, sort by timer ID, and acquire every `it_lock` in deterministic order.

### Step 4: Implement timer metadata capture

With all timer locks held, then reacquiring `siglock`, copy:

- timer ID and clock ID;
- notify mode and signal number;
- `sigq->info.si_value.sival_ptr`;
- target TID;
- interval and remaining time through the timer clock's `timer_get` callback;
- overrun fields.

Do not call `common_timer_get()` without its `it_lock`. Revalidate timer ownership and ID set before accepting the capture.

### Step 5: Handle overrun according to the approved scope

Serialize overrun into A6 snapshot and CRIU `posix_timer_entry.overrun`, but add no claim that stock CRIU restores `timer_getoverrun()` exactly. Add a test that confirms the field is present in the generated protobuf.

### Step 6: Add timer fixture

`timers.c` must cover:

- all three interval timers;
- one-shot and periodic POSIX timers;
- `SIGEV_SIGNAL` and `SIGEV_THREAD_ID`;
- different clock IDs accepted by the first scope;
- a controlled overrun value for image inspection;
- a timer close to expiry.

### Step 7: Build and test

```bash
make -C tests/progs clean all
make -C kernel_module clean all KDIR=/home/yhome.guest/kernels/linux-5.10.29
./tests/a6-abi-contract.sh --timer-locks
```

Expected: module builds without atomic-sleep or lock-order diagnostics.

### Step 8: Commit

```bash
git add kernel_module/checkpoint/dump_timers.* kernel_module/checkpoint/dump_task.c tests/progs/timers.c tests/progs/Makefile tests/a6-abi-contract.sh
git commit -m "feat: collect interval and POSIX timer state"
```

## Task 5: Integrate A6 collection into dump transaction

**Files:**
- Modify: `kernel_module/checkpoint/dump.c`
- Modify: `kernel_module/checkpoint/snapshot_writer.c`
- Modify: `kernel_module/checkpoint/snapshot_writer.h`
- Modify: `include/criu_snapshot.h`
- Test: `tests/dump-errors.sh`, `tests/snapshot-writer.sh`

### Step 1: Add a failing transaction test

Force an A6 collector or record write error and verify:

- `snapshot.bin` is absent;
- `snapshot.bin.tmp` is absent;
- target is thawed;
- freeze state returns idle;
- no partial A6 record is accepted by the converter.

### Step 2: Set the A6 header flag only after successful capture

The header capability flag must be determined before writing the header. Therefore the dump path must complete the in-memory A6 preflight/capture contract before opening the writer, or hold the flag in a dump context and abort if final capture fails. Do not publish a header claiming A6 data when any record is missing.

### Step 3: Emit A6 records in deterministic order

Use this order before `REC_END`:

```text
TASK / REGS / CREDS
THREAD records
MM / VMA / PAGE_RUN
FD and object records
SIGACTION
shared SIGNAL_QUEUE
private SIGNAL_QUEUE sorted by TID
ITIMER_SET
POSIX_TIMER_TABLE
REC_END
```

The reader must not depend on order, but deterministic output simplifies checksums and fixture comparison.

### Step 4: Run regression tests

```bash
./tests/snapshot-writer.sh
./tests/dump-errors.sh
./tests/a4-thread-contract.sh
```

Expected: existing A3–A5 transaction and rollback behavior remains unchanged.

### Step 5: Commit

```bash
git add kernel_module/checkpoint/dump.c kernel_module/checkpoint/snapshot_writer.* include/criu_snapshot.h tests/dump-errors.sh tests/snapshot-writer.sh
git commit -m "feat: publish A6 records transactionally"
```

## Task 6: Extend converter model and strict A6 validation

**Files:**
- Modify: `userspace/criu-module-convert/criu_model.c`
- Modify: `userspace/criu-module-convert/criu_model.h`
- Modify: `userspace/criu-module-convert/snapshot_reader.c`
- Modify: `tests/fixtures/a6-snapshot-builder.py`
- Modify: `tests/a6-converter-images.sh`

### Step 1: Add failing model tests

Generate a valid A6 snapshot and assert the converter currently rejects or ignores its signal/timer records. Add malformed fixtures for every Phase 1 rule.

Run:

```bash
./tests/a6-converter-images.sh
```

Expected: FAIL before model fields and validators are implemented.

### Step 2: Add model storage and indexing

Add blob/list storage for:

- sigaction table;
- signal queue chunks;
- interval timer table;
- POSIX timer table.

Build indexes keyed by `(scope, owner_tid)` and timer ID. Preserve raw bytes; do not parse away unknown `siginfo` union data.

### Step 3: Implement A6 validation

When the header flag is set, require all A6 families and reject:

- missing/duplicate tables;
- incomplete thread queue coverage;
- invalid queue chunk intervals;
- bad siginfo sizes or signal numbers;
- timer target TIDs outside the frozen set;
- inconsistent legacy bitsets;
- invalid timer ranges, precision, or duplicate IDs.

When the flag is absent, retain the legacy converter path and emit an explicit diagnostic that the output is not an A6 gate result.

### Step 4: Run negative and positive tests

```bash
make -C userspace/criu-module-convert clean all
./tests/a6-converter-images.sh
./tests/converter-format.sh
```

Expected: valid A6 fixture passes; every malformed fixture fails without creating output images.

### Step 5: Commit

```bash
git add userspace/criu-module-convert/criu_model.* userspace/criu-module-convert/snapshot_reader.c tests/fixtures/a6-snapshot-builder.py tests/a6-converter-images.sh
git commit -m "feat: validate A6 signal and timer records"
```

## Task 7: Generate CRIU core protobuf fields

**Files:**
- Modify: `userspace/criu-module-convert/criu_model.c`
- Modify: `userspace/criu-module-convert/image_writer.c` only if a missing wire helper is required
- Modify: `tests/a6-converter-images.sh`

### Step 1: Add failing protobuf assertions

Decode generated `core-$tid.img` messages in Python and assert the expected fields are absent or default before implementation.

### Step 2: Implement sigaction mapping

Replace default 62-entry construction in A6 mode with real `sa_entry` values:

- handler;
- flags;
- restorer;
- mask;
- optional extended mask only when non-zero and supported.

Skip only SIGKILL/SIGSTOP after validating their raw entries.

### Step 3: Implement shared/private signal queue mapping

Build `signal_queue_entry` with one nested `siginfo_entry` per validated raw 128-byte payload. Put shared data in leader task core field 10 and private data in the matching thread core field 9.

### Step 4: Implement interval and POSIX timer mapping

Build `task_timers_entry` fields 1–4 from validated records. Preserve overrun in each `posix_timer_entry`, use CRIU’s expired-timer minimum-nonzero convention, and sort POSIX entries by ID.

### Step 5: Preserve atomic directory publication

Rename the staging suffix from `.a5-tmp.XXXXXX` to `.criu-module-tmp.XXXXXX` while preserving existing fsync, rename, and cleanup behavior.

### Step 6: Run image assertions

```bash
make -C userspace/criu-module-convert clean all
./tests/a6-converter-images.sh
./tests/converter-images.sh
./tests/a5-images.sh
```

Expected: A6 protobuf fields match the fixture exactly; A3–A5 image tests remain PASS.

### Step 7: Commit

```bash
git add userspace/criu-module-convert/criu_model.c userspace/criu-module-convert/image_writer.c tests/a6-converter-images.sh
git commit -m "feat: emit A6 signal and timer core images"
```

## Task 8: Add guest behavior fixtures and unsupported paths

**Files:**
- Create: `tests/a6-cross-restore.sh`
- Create: `tests/a6-unsupported.sh`
- Create or modify: `tests/progs/sig-handlers.c`
- Create or modify: `tests/progs/sig-pending.c`
- Create or modify: `tests/progs/timers.c`
- Modify: `tests/progs/Makefile`

### Step 1: Add unsupported behavior cases

Explicitly reject and verify rollback for:

- unsupported timer clock/notify combinations;
- target TID outside the frozen thread group;
- malformed pending state;
- queue size beyond the documented bound;
- namespace-dependent notify IDs in the A6-only environment.

Run in the target guest:

```bash
./scripts/run-qemu.sh --ci --script tests/a6-unsupported.sh
```

Expected: each case reports explicit unsupported, target resumes, no snapshot or `.tmp` remains, and dmesg has no warning/oops/atomic-sleep/lock-inversion diagnostics.

### Step 2: Add cross-restore behavior checks

The script must:

1. start a fixture and wait for its ready marker;
2. set the debugfs target;
3. invoke module dump to a guest-local `/tmp` path;
4. run the converter;
5. invoke real `criu restore` in the guest;
6. verify the restored PID remains alive;
7. trigger/unblock signals and observe handler markers;
8. verify private versus shared delivery;
9. wait for interval/POSIX timer markers and second responses;
10. preserve logs and classify missing CRIU or guest setup as environment skip, not feature PASS.

### Step 3: Run guest scripts

```bash
./scripts/run-qemu.sh --ci --script tests/a6-cross-restore.sh
./scripts/run-qemu.sh --ci --script tests/a6-unsupported.sh
```

Expected: `A6_CROSS_RESTORE: PASS` only after behavior checks; otherwise a precise `SKIP`, `UNSUPPORTED`, or `FAIL` classification.

### Step 4: Commit

```bash
git add tests/a6-cross-restore.sh tests/a6-unsupported.sh tests/progs/sig-handlers.c tests/progs/sig-pending.c tests/progs/timers.c tests/progs/Makefile
git commit -m "test: add A6 guest signal and timer gates"
```

## Task 9: Full A6 regression and release evidence

**Files:**
- Create: `tests/a6-regression.sh`
- Modify: `docs/steps/A6-signals-timers.md`
- Modify: `docs/03-Iteration-Plan.md`
- Create: `docs/plans/2026-09-18-a6-verification.md`

### Step 1: Run local contract and converter suite

```bash
./tests/a6-abi-contract.sh
./tests/a6-converter-images.sh
./tests/a6-unsupported.sh
./tests/converter-images.sh
./tests/a4-cross-restore.sh
./tests/a5-cross-restore.sh
```

### Step 2: Run the authoritative nested guest matrix

```bash
./scripts/run-qemu.sh --ci --script tests/a6-cross-restore.sh
./scripts/run-qemu.sh --ci --script tests/a6-regression.sh
```

Record fresh commands and output, including guest kernel version, module build identity, CRIU identity, and dmesg diagnostics.

### Step 3: Update documentation

Document:

- the modern core-embedded image mapping;
- the `CRIU_SNAPSHOT_F_SIGNAL_TIMERS` contract;
- the `it_lock -> siglock` order;
- synthetic bit-only siginfo behavior;
- the approved overrun boundary;
- unsupported extension items and their A9-after scheduling.

### Step 4: Commit verification evidence

```bash
git add tests/a6-regression.sh docs/steps/A6-signals-timers.md docs/03-Iteration-Plan.md docs/plans/2026-09-18-a6-verification.md
git commit -m "docs: record A6 verification and scope boundary"
```

## Completion criteria

A6 is complete for its first gate only when all of the following are true:

- valid A6 snapshot ABI fixtures pass and malformed fixtures fail transactionally;
- module builds against Linux 5.10.29 without atomic-sleep or lock-order diagnostics;
- sigaction, shared/private pending, interval timers, and POSIX timer metadata survive conversion;
- overrun is present in snapshot and CRIU image, with the exact runtime restore limitation documented;
- nested guest real CRIU restore passes post-restore signal/timer behavior checks;
- A3/A4/A5 regression gates remain PASS;
- unsupported states are explicit and do not leave partial output;
- verification document contains fresh reproducible commands and outputs.

The following do not block this first gate and remain post-A9 extensions: exact `timer_getoverrun()` runtime restoration, fown signal semantics, signalfd/timerfd, cross-namespace targets, and full ZDTM/GitHub CI.

