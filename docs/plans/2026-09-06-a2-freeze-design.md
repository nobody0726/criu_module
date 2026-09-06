# A2 Freeze/Thaw Spike Design

**Date:** 2026-09-06  
**Status:** Approved for implementation planning  
**Scope:** A2 feasibility spike; no dump or restore image format

## Goal

Provide a reliable, synchronous freeze/thaw primitive for the complete thread
group selected by A1's `target` debugfs file. A successful freeze must leave the
address space stable and all thread registers settled so later dump readers can
take a consistent snapshot. The first spike does not implement descendants;
process-tree freezing is a separate second-stage experiment.

## Decisions

- Control plane: the kernel module owns the operation through debugfs.
- Mechanism: use the Linux 5.10.29 cgroup v2 freezer through a small, project-
  maintained GPL kernel wrapper. The wrapper is patched into `kernel/cgroup`
  because the v2 freezer and cgroup migration primitives are not exported to
  out-of-tree modules in the stock 5.10.29 tree. Do not silently fall back to
  per-task freezer APIs.
- Target selection: reuse A1's pinned `target` and generation state.
- `freeze` is synchronous. It returns only after every target thread is settled,
  or after rollback on error/timeout.
- Freeze scope: target PID's thread group only. Add independent
  `freeze_tree`/`thaw_tree` interfaces in the later descendants experiment.
- Required privilege: `CAP_SYS_ADMIN`; the spike guest must provide cgroup v2
  and `CONFIG_CGROUPS`. `CONFIG_CGROUP_FREEZER` is the legacy v1 option and is
  not a v2 capability gate.
- Target replacement is rejected with `-EBUSY` while a freeze context exists.
- Any failure, including settled timeout, performs a complete rollback.
- The state present before freezing is part of the contract: every thread's
  original stopped state is recorded and restored by `thaw`.

## Components and ownership

The A1 target state remains the source of truth for selecting a task. A2 adds a
freeze context that owns all resources for one operation:

```text
target PID
   |
   v
pinned task + generation
   |
   v
freeze_ctx
  - target task/thread-group references
  - original cgroup path and pinned cgroup reference
  - temporary freezer cgroup reference
  - per-thread references and original stopped states
  - state, deadline, and last error
```

The context, rather than a numeric PID, is used by `thaw` and by all status
reporting. This prevents PID reuse from thawing an unrelated task.

## Kernel wrapper boundary

The stock 5.10.29 kernel declares the v2 freezer helpers in headers but does
not export them, and it does not export `cgroup_attach_task()`. A2 therefore
ships a small source patch under `patches/linux-5.10.29/`. The patch adds a GPL
exported high-level wrapper in the cgroup core. Its opaque cookie owns the
temporary cgroup and the pinned original cgroup reference:

```c
struct criu_freezer_cookie;

int criu_cgroup_freeze_threadgroup(struct task_struct *leader,
                                   struct criu_freezer_cookie **cookie,
                                   char *original_path, size_t original_len,
                                   char *temporary_path, size_t temporary_len);
int criu_cgroup_thaw_threadgroup(struct criu_freezer_cookie *cookie);
```

The wrapper runs inside the kernel's cgroup locking discipline and calls the
private `cgroup_create()`, `cgroup_attach_task()`, and `cgroup_freeze()` logic.
The module never reaches into `cgroup-internal.h`, never uses kallsyms, and
never creates or destroys a cgroup by emulating kernfs writes. The build applies
the patch before compiling the module; the probe verifies the wrapper's GPL
export with modpost.

## Debugfs interface

Existing A1 files keep their semantics. A2 adds:

- `freeze`: write an empty value or `1` to freeze the current target's thread
  group synchronously.
- `thaw`: write an empty value or `1` to reverse the active freeze context. With
  no context it returns `-ENOENT`.
- `status`: add the freeze fields below. `freeze_tree` and `thaw_tree` are not
  registered during this spike.

The status output is a snapshot and must not mix target generations or contexts
within one read:

```text
freeze_state=idle|freezing|frozen|thawing|rollback
freeze_generation=<target generation>
freeze_task_count=<number of pinned threads>
freeze_settled=0|1
freeze_was_stopped=0|1
freeze_last_error=<negative errno or 0>
freeze_cgroup_original=<path or unavailable>
freeze_cgroup_temporary=<path or unavailable>
```

While a context exists, writing `target` returns `-EBUSY`. The same rule applies
to a second `freeze` request. Every control and read/open operation continues
to require `CAP_SYS_ADMIN`.

## Freeze lifecycle

```text
IDLE
  -> FREEZING
  -> FROZEN_SETTLED
  -> THAWING
  -> IDLE

FREEZING --error/timeout--> ROLLING_BACK --> IDLE
```

On `freeze`:

1. Lock target state and reject an absent target or an existing context.
2. Pin the target task, enumerate and pin every thread in its thread group, and
   record each thread's original stopped state.
3. Record the original cgroup path and create/select a temporary cgroup v2
   freezer.
4. Move the pinned thread group to the temporary cgroup and request frozen
   state.
5. Poll `criu_freeze_settled()` until all tasks are off-CPU, or until the
   deadline expires.
6. Publish `FROZEN_SETTLED` only after the complete set has settled.

On `thaw`, reverse the operation using the context's pinned references: release
the temporary freezer state, restore the original cgroup membership, restore
the recorded stopped states, then release task/cgroup references and return to
`IDLE`.

## Settled definition and timing

The cgroup freezer's frozen state is not by itself proof that registers have
been saved from hardware. `criu_freeze_settled()` must inspect every pinned
thread and reject a task that is still running or runnable. The caller polls at
10 ms intervals with a default 5 s timeout. The timeout is a module parameter so
tests can set `settle_timeout_ms=0` and deterministically exercise rollback.

The public kernel-facing interfaces are:

```c
struct criu_freeze_ctx;

int criu_freeze(pid_t vpid, bool include_children,
                struct criu_freeze_ctx **ctx);
void criu_thaw(struct criu_freeze_ctx *ctx);
bool criu_freeze_settled(struct criu_freeze_ctx *ctx);
```

For this spike, callers pass `include_children=false`; the tree experiment will
define separate entry points instead of expanding the first state machine.

## Error and rollback contract

- `-EPERM`: caller lacks `CAP_SYS_ADMIN`.
- `-ESRCH`: no valid target, target exited, or target has no usable `mm`.
- `-EBUSY`: active freeze context or target replacement during a freeze.
- `-ETIMEDOUT`: settled polling exceeded its deadline.
- `-EOPNOTSUPP`: cgroup v2 freezer, wrapper patch, or required configuration is
  unavailable.
- `-EINVAL`: malformed control-file input.
- `-ENOENT`: `thaw` requested without an active context.

All failures use one rollback path:

```text
mark ROLLBACK
  -> thaw tasks already moved to the temporary cgroup
  -> restore original cgroup membership
  -> restore original stopped states
  -> release all task/cgroup references
  -> state=IDLE and record last_error
```

No partial freeze is left for the caller to clean up.

## Validation plan

The guest-only tests use the existing `scripts/run-qemu.sh` workflow and never
load the module on the development host.

### Fixtures

- `busy-counter`: one CPU-bound process with a shared monotonic counter.
- `multithread-counter`: several threads with independent heartbeat slots.
- `stopped-counter`: a thread group placed in its original stopped state before
  the target is selected.
- `fork-bomb-lite`: reserved for the second-stage tree experiment; the first
  stage verifies it is not accidentally included.

### Gates

- `freezer-symbols.sh`: compile/configuration gate for cgroup v2 freezer support.
- `freeze-test.sh`: success path, counter quiescence, all-thread settled state,
  thaw continuation, cgroup restoration, and stopped-state preservation.
- `freeze-errors.sh`: absent target, permission checks, duplicate operations,
  target replacement, target exit, and malformed writes.
- `freeze-rollback.sh`: deterministic timeout, complete rollback, unchanged
  cgroup path, no residual context, and clean reload/unload.

### Acceptance criteria

1. Single-thread and multi-thread groups freeze synchronously.
2. No heartbeat advances while `freeze` reports success.
3. `criu_freeze_settled()` is true for every pinned thread at success.
4. `thaw` resumes running groups and preserves initially stopped groups.
5. Target generation, task identity, and cgroup membership are restored.
6. Every failure rolls back without leaks, stale debugfs state, or dmesg warnings.
7. The first-stage module does not expose descendants/tree controls.

## Risks and explicit non-goals

The primary feasibility risk is whether the maintained wrapper patch applies
cleanly and preserves the 5.10.29 cgroup locking invariants. The compile gate
must make wrapper availability explicit before implementation proceeds. This spike does not dump memory,
serialize task credentials/files/signals, handle descendants, or define a CRIU
image format.
