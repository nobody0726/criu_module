# S0 Feasibility Spike Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在隔离的 Linux 5.10.29 ARM64 QEMU guest 中完成 S0 文档规定的全部 1.1-4.8 实验，形成可追溯结论，并回填内核模块边界文档。

**Architecture:** 使用清单驱动的临时 spike 外置模块。compile probe 独立构建并通过 modpost 判定导出状态；runtime probe 每项启动全新的 QEMU guest，由 guest 加载模块、读取 debugfs 和 procfs、检查 dmesg 后卸载模块。Lima guest 只负责编译和编排，绝不直接 insmod。

**Tech Stack:** C、Linux kernel module API、Linux 5.10.29 ARM64、QEMU、BusyBox initramfs、POSIX shell、debugfs、GitHub Actions。

---

## Execution Rules

- 本计划在 codex/s0-feasibility-spike 分支执行。
- 参考源码固定为 /Users/yhome/workspace/source_code/linux-5.10.29/，只读。
- 实际构建树为 $HOME/kernels/linux-5.10.29；必须与 QEMU 启动的内核一致。
- 所有模块加载动作只能发生在 scripts/run-qemu.sh 创建的 QEMU guest 中。
- 每个任务完成后运行本任务列出的验证命令，再提交一个小而明确的 commit。
- 不使用 kallsyms_lookup_name()、kprobe_lookup_name() 或 mm->mmap_sem。
- 生成的 .o、.ko、.cmd、Module.symvers、modules.order 和实验日志不提交 Git。
- 如果发现用户已有未提交改动，先保留并只操作本计划涉及的文件。

## Result Contract

内部状态为 OK、UNSAFE、NO-SYMBOL、WRONG-VALUE、CRASH、CLEANUP-FAIL、ENV-MISMATCH。
最终 docs/principles/08-kernel-module-limits.md 仍使用文档规定的 OK、UNSAFE、
NO-SYMBOL；后三种运行失败作为 UNSAFE 的原因保存。

NO-SYMBOL 只允许用于目标符号被编译器或 modpost 明确报告无法解析的情况。配置、
头文件或测试框架错误必须先修复，不能伪装成符号不可用。

## Task 1: Establish the Environment and Source Audit

Files:
- Create: tests/s0/source-audit.sh
- Create: tests/s0/check-environment.sh
- Modify: .gitignore for artifacts/s0/ and S0 transient files
- Reference only: /Users/yhome/workspace/source_code/linux-5.10.29/

Step 1: Write the failing environment checks.

Implement checks that fail unless uname -m is aarch64, the configured KDIR reports
5.10.29, and the required config values are present. Require CONFIG_ARM64_PTR_AUTH to
be unset. Emit ENV-MISMATCH and exit non-zero on mismatch.

Step 2: Run the checks against the current environment.

Run:

~~~sh
KDIR="$HOME/kernels/linux-5.10.29" tests/s0/check-environment.sh
~~~

Expected: PASS, with kernel release, architecture, config hash, and KDIR printed.

Step 3: Implement the read-only source audit.

Make source-audit.sh accept LINUX_SRC and write one normalized record per target API:
declaration location, export location, export flavor, and relevant source note. Cover:

~~~text
find_get_task_by_vpid find_vpid pid_task get_pid_task put_task_struct
get_task_mm mmput mmget_not_zero mmap_read_lock
vm_next vm_file d_path follow_page get_user_pages_remote access_process_vm
mm_alloc vm_area_alloc insert_vm_struct do_mmap vm_mmap do_munmap vm_munmap
alloc_pid kernel_execve vm_insert_page
~~~

Use rg and awk over the fixed source tree; do not edit it. Explicitly record that vm_mmap()
and vm_munmap() are current-mm wrappers and that vm_insert_page() can set VM_MIXEDMAP.

Step 4: Run the source audit and inspect the output.

Run:

~~~sh
LINUX_SRC=/Users/yhome/workspace/source_code/linux-5.10.29 \
  tests/s0/source-audit.sh > /tmp/s0-source-audit.txt
~~~

Expected: every target has a record, and the output distinguishes MMU implementations
from nommu.c.

Step 5: Commit the baseline tooling.

~~~sh
git add tests/s0/source-audit.sh tests/s0/check-environment.sh .gitignore
git commit -m "test: add S0 environment and source audit" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 2: Add the Known-Layout ARM64 Fixture

Files:
- Create: tests/progs/known-layout.c
- Create: tests/progs/Makefile

Step 1: Implement the fixture.

Create two page-aligned mappings with mmap():

- private anonymous read/write page filled with 0xa5;
- shared anonymous read/write page filled with 0x5a.

Create a PROT_NONE anonymous page and a separate ordinary anonymous insertion
candidate. Print one parseable line containing PID and all four addresses, flush stdout,
then call pause() in a loop. Return non-zero on any mapping or page-touch error.

Step 2: Build it as static ARM64 code.

Run inside Lima:

~~~sh
make -C tests/progs clean all
file tests/progs/known-layout
~~~

Expected: static aarch64 executable. If the host compiler is not ARM64, use the guest's
native compiler; do not introduce cross-architecture behavior into the fixture.

Step 3: Run the fixture without loading any module.

Run:

~~~sh
tests/progs/known-layout > /tmp/known-layout.out &
pid=$!
sleep 1
cat /tmp/known-layout.out
grep -q "pid=$pid" /tmp/known-layout.out
kill "$pid"
wait "$pid" 2>/dev/null || true
~~~

Expected: one line with valid non-zero addresses and the process remains alive until killed.

Step 4: Commit the fixture.

~~~sh
git add tests/progs/known-layout.c tests/progs/Makefile
git commit -m "test: add S0 known-layout process fixture" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 3: Build the Probe Framework and Guest Protocol

Files:
- Create: spike/Makefile
- Create: spike/criu_spike.c
- Create: spike/criu_spike.h
- Create: tests/s0/probes.tsv
- Create: tests/s0/guest-run-one.sh

Step 1: Define the probe manifest.

Add one row for every required item:

~~~text
1.1 1.2 1.3 1.4
2.1 2.2 2.3 2.4 2.5
3.1 3.2 3.3 3.4 3.5
4.1 4.2 4.3 4.4a 4.4b 4.5a 4.5b 4.6 4.7 4.8
~~~

Include columns for kind, symbol, expected result, gate flag, and fixture address needs.
Use tabs and reject malformed rows in the parser.

Step 2: Implement the common module contract.

Implement module parameters, GPL license, debugfs root, report and VMA seq files,
status handling, and bounded report buffers. Check target_pid and all addresses before
use. Use one cleanup path that releases task, pid, mm, page, and debugfs resources.
The module must never look up symbols dynamically.

Step 3: Implement the guest protocol.

guest-run-one.sh must:

1. require UID 0 and 5.10.x;
2. mount debugfs and verify /mnt/host;
3. start known-layout, parse PID and all addresses, and save proc maps and smaps;
4. record dmesg line count before the probe;
5. load the selected module with manifest parameters;
6. read debugfs outputs;
7. execute probe-specific comparisons;
8. unload the module and kill the fixture;
9. detect new kernel warnings and write S0_RESULT.

Use traps for cleanup. A missing result marker is a crash or cleanup failure, not a pass.

Step 4: Verify the framework compiles.

Run:

~~~sh
make -C spike KDIR="$HOME/kernels/linux-5.10.29" PROBE=2.2
~~~

Expected: the framework builds without probe-specific implementation yet; the guest
script rejects an absent probe report rather than claiming success.

Step 5: Commit the framework.

~~~sh
git add spike tests/s0/probes.tsv tests/s0/guest-run-one.sh
git commit -m "test: add S0 probe framework and guest protocol" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 4: Implement and Run Group 1 Task Probes

Files:
- Modify: spike/criu_spike.c
- Create: spike/probes/task.c
- Modify: tests/s0/guest-run-one.sh

Step 1: Add compile probe 1.1.

Reference find_get_task_by_vpid() alone and build it alone. Capture compiler and modpost
output. Expected result is NO-SYMBOL if the 5.10.29 build tree cannot resolve it.

Step 2: Add runtime probe 1.2.

Under rcu_read_lock(), call find_vpid(target_pid) and pid_task(pid, PIDTYPE_PID).
Report task PID, TGID and comm. Do not retain the task pointer beyond the RCU critical
section. The guest compares PID and comm with the fixture.

Step 3: Add runtime probe 1.3.

Call find_vpid(), then get_pid_task(), report task identity, call put_task_struct(),
and verify repeated module load/unload leaves no warning.

Step 4: Add probe 1.4 as an intentional contract violation.

Call task lookup without rcu_read_lock(). Capture any suspicious RCU usage output.
Regardless of whether this debug build emits a warning, classify the probe as UNSAFE with
reason missing RCU protection.

Step 5: Run the group.

Run:

~~~sh
tests/s0/run-all.sh --group 1
~~~

Expected: 1.1 is NO-SYMBOL; 1.2 and/or 1.3 is OK; 1.4 is UNSAFE by design. A failure
of both 1.2 and 1.3 is an A-track blocker.

Step 6: Commit group 1.

~~~sh
git add spike tests/s0/guest-run-one.sh
git commit -m "test: probe task lookup boundaries for S0" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 5: Implement and Run Group 2 MM/VMA Probes

Files:
- Modify: spike/criu_spike.c
- Create: spike/probes/mm.c
- Modify: tests/s0/guest-run-one.sh

Step 1: Add compile probe 2.1.

Reference mmget_not_zero() alone and classify an unresolved module symbol as NO-SYMBOL.

Step 2: Add probe 2.2.

Obtain a referenced task, call get_task_mm(), report non-null mm and mm_users, then call
mmput() on every path after successful acquisition.

Step 3: Add probe 2.3.

Hold mmap_read_lock(mm) only while reading bounded fields, release it before mmput(), and
let the dmesg checker catch lockdep or sleep violations.

Step 4: Add probe 2.4.

With the read lock held, walk mm->mmap through vm_next. Emit start/end/permission fields,
count VMAs, identify the fixture mappings, and read their first bytes through a selected
safe page-access path. Never dereference a VMA after releasing the lock.

Step 5: Add probe 2.5.

For file-backed VMAs, use a bounded page buffer and d_path() on vm_file->f_path. Return
the buffer to the allocator on every path. Compare paths with proc/PID/maps; anonymous
VMAs are allowed to have no path.

Step 6: Run group 2 and enforce the gate.

Run:

~~~sh
tests/s0/run-all.sh --group 2
~~~

Expected: 2.1 NO-SYMBOL; 2.2, 2.3 and 2.5 OK if the API and comparison succeed; 2.4 OK
only when count, ranges, permissions, magic bytes and dmesg are all clean.

Step 7: Commit group 2.

~~~sh
git add spike tests/s0/guest-run-one.sh
git commit -m "test: probe 5.10 task mm and VMA access" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 6: Implement and Run Group 3 Page Access Probes

Files:
- Modify: spike/criu_spike.c
- Create: spike/probes/pages.c
- Modify: tests/s0/guest-run-one.sh

Step 1: Add compile probe 3.1.

Reference follow_page() alone. Record the exact modpost error and classify only that
error as NO-SYMBOL.

Step 2: Add probe 3.2.

Call get_user_pages_remote() using the exact 5.10.29 signature from the reference tree,
with the required mmap lock held. Request one page, read its first byte through the page
mapping API, call put_page(), and report the return value and byte.

Step 3: Add probe 3.3.

Call access_process_vm() for the private and shared fixture pages. Report byte counts and
first bytes; expected values are 0xa5 and 0x5a.

Step 4: Add probes 3.4 and 3.5.

For 3.4 use a page-aligned address with no covering VMA and require an error/zero-length
return without a kernel exception. For 3.5 call both APIs on the PROT_NONE address and
record actual return values; no fixed return value is required, but no debug-kernel
complaint is allowed.

Step 5: Run group 3.

Run:

~~~sh
tests/s0/run-all.sh --group 3
~~~

Expected: 3.1 NO-SYMBOL; 3.2 or 3.3 at least OK; 3.4 and 3.5 have predictable
non-crashing results. Any leaked page reference or dmesg error is failure evidence.

Step 6: Commit group 3.

~~~sh
git add spike tests/s0/guest-run-one.sh
git commit -m "test: probe remote user page access for S0" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 7: Implement and Run Group 4 Restore-Boundary Probes

Files:
- Modify: spike/criu_spike.c
- Create: spike/probes/restore.c
- Modify: tests/s0/guest-run-one.sh

Step 1: Add compile probes 4.1-4.7.

Build each target in isolation: mm_alloc, vm_area_alloc, insert_vm_struct, do_mmap,
do_munmap, alloc_pid and kernel_execve. Record exact declaration, link and modpost
outcome. do_mmap and do_munmap must be tested separately from vm_mmap and vm_munmap.

Step 2: Add positive wrapper probes.

For 4.4b and 4.5b, verify the exported wrapper can link and report that it acts on
current->mm. Do not use it to mutate the fixture target mm. If a runtime demonstration
is needed, use only a disposable current task mapping and unmap it before exit.

Step 3: Add probe 4.8.

Find the insert_addr VMA, save vm_flags, allocate a clean individual page, acquire
mmap_write_lock(mm), call vm_insert_page(), release the lock, and report return value
and flags before/after. Save smaps before and after from the guest. Always clean up page
references and target process state.

Step 4: Classify the side effect.

If VM_MIXEDMAP is added to an ordinary anonymous VMA, classify as UNSAFE even when the
call returns zero. The report must include flag delta and smaps delta. This is a semantic
rejection, not a link failure.

Step 5: Run group 4.

Run:

~~~sh
tests/s0/run-all.sh --group 4
~~~

Expected: 4.1, 4.2, 4.3, 4.4a, 4.5a, 4.6 and 4.7 are NO-SYMBOL if the actual build
tree confirms it; 4.4b and 4.5b document current-mm scope; 4.8 is UNSAFE with
VM_MIXEDMAP evidence.

Step 6: Commit group 4.

~~~sh
git add spike tests/s0/guest-run-one.sh
git commit -m "test: verify S0 restore boundary and VMA side effects" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 8: Add the Complete Orchestrator and Evidence Summarizer

Files:
- Create: tests/s0/run-all.sh
- Create: tests/s0/run-one.sh
- Create: tests/s0/check.sh
- Create: tests/s0/summarize.sh
- Modify: tests/s0/probes.tsv if manifest metadata is incomplete
- Modify: .gitignore

Step 1: Implement run directory creation.

Create artifacts/s0/<UTC-run-id>/ and save environment and source-audit outputs before
running probes. Allow --keep-workdir; otherwise remove only known transient staging files,
never the repository or kernel tree.

Step 2: Implement compile probe execution.

For each compile row, clean only spike build products, invoke make with one PROBE, capture
stdout/stderr, identify modpost unresolved symbols, and write result.txt. Do not continue
to runtime when compilation fails.

Step 3: Implement runtime probe execution.

For each runtime row, build the selected module, invoke scripts/run-qemu.sh --ci --script
tests/s0/guest-run-one.sh, apply a bounded timeout, and copy all guest artifacts into the
probe directory. If QEMU exits without S0_RESULT, classify as CRASH or CLEANUP-FAIL based
on the saved log.

Step 4: Implement the result checker.

check.sh must reject missing manifest rows, missing evidence, missing result markers, dirty
dmesg for probes expected safe, surviving debugfs nodes, and unexpected module load/unload
failures. Treat 1.4 and 4.8 as intentional unsafe probes with explicit reasons.

Step 5: Implement summary output.

Generate summary.tsv with ID, group, expected result, observed result, kernel, config hash,
and evidence path. Print a concise table for humans. Return non-zero when acceptance gates
fail, while preserving all evidence.

Step 6: Run the complete matrix once.

Run:

~~~sh
tests/s0/run-all.sh
tests/s0/check.sh artifacts/s0/<run-id>
~~~

Expected: one result for every row and no missing-evidence errors. The aggregate may report
intentional UNSAFE probes; acceptance is based on S0 criteria, not zero unsafe results.

Step 7: Commit the orchestrator.

~~~sh
git add tests/s0 .gitignore
git commit -m "test: automate complete S0 probe matrix" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 9: Repeat Critical Probes and Resolve Inconsistent Results

Files:
- Modify: tests/s0/run-all.sh if repeat selection needs support
- Create: artifacts/s0/<run-id>/repeat-summary.tsv (ignored)

Step 1: Repeat the critical runtime set.

Run each three times in new QEMU guests:

~~~text
1.2 2.4 3.2 3.3 4.8
~~~

Preserve each attempt's complete evidence.

Step 2: Compare results.

Run:

~~~sh
tests/s0/summarize.sh artifacts/s0/<run-id>
~~~

Expected: each repeated probe has stable status and stable key values. Any divergence is
classified UNSAFE with reason non-deterministic result.

Step 3: Resolve only framework-caused failures.

If a failure is due to fixture parsing, stale generated output, QEMU protocol, or cleanup
logic, fix the framework and rerun the affected probe. If it is a real kernel warning,
wrong value, crash, or semantic side effect, preserve it as evidence.

Step 4: Commit only necessary fixes.

~~~sh
git add tests/s0 spike
git commit -m "test: stabilize S0 critical probe validation" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 10: Write Conclusions into the Kernel-Module Boundary Docs

Files:
- Modify: docs/principles/08-kernel-module-limits.md
- Modify: docs/steps/S0-feasibility-spike.md only if implementation details materially differ
- Create: artifacts/s0/<final-run-id>/final-summary.tsv (ignored)

Step 1: Generate the final summary.

Run the complete matrix and critical repeats after all fixes:

~~~sh
tests/s0/run-all.sh
tests/s0/check.sh artifacts/s0/<final-run-id>
~~~

Step 2: Fill the experiment table.

For every row in section 8 of docs/principles/08-kernel-module-limits.md, write observed
conclusion, exact kernel release and arm64 architecture, key config values/config hash, UTC
date, short evidence path, and relevant error or flag delta.

Keep source-audit conclusion separate from runtime result. Do not replace measured results
with earlier expectations.

Step 3: Record architectural consequences.

State explicitly whether A-track task lookup, mm acquisition, VMA traversal and page reading
are viable. If group 1 or group 2 has no viable path, mark A-track blocked and stop before
starting A1. Record fallback choices: B-track, kernel patch, or X1.

Step 4: Run documentation and repository checks.

~~~sh
git diff --check
git grep -nE 'kallsyms_lookup_name|kprobe_lookup_name' -- spike tests/s0 || true
git grep -nE '\bmm->mmap_sem\b' -- spike tests/s0 || true
~~~

Expected: no forbidden symbol lookup or mmap_sem matches.

Step 5: Commit the conclusions.

~~~sh
git add docs/principles/08-kernel-module-limits.md docs/steps/S0-feasibility-spike.md
git commit -m "docs: record S0 feasibility results" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Task 11: Remove One-Time Spike Code and Verify the Repository

Files:
- Delete: spike/ and S0 runtime implementation files after conclusions are committed
- Delete: tests/s0/ and tests/progs/ only if the final decision treats them as disposable
- Keep: this plan and the approved design document
- Modify: .gitignore only as needed to remove obsolete transient patterns

Step 1: Decide the retained artifact boundary.

Retain only the final conclusion table, design document, and implementation plan by default.
If a fixture or runner is useful for future regression, retain it under clearly marked
tests/s0/ with a README saying it is a one-time feasibility harness and must not be reused
as A1 implementation. Record the decision in the final commit.

Step 2: Remove generated and temporary files.

Delete only known S0 build products and artifacts using explicit paths. Confirm the kernel
source tree and user files are untouched. Do not use broad recursive deletion against the
workspace or home directory.

Step 3: Run existing project checks.

~~~sh
git diff --check
git status --short --branch
make -C kernel_module KDIR="$HOME/kernels/linux-5.10.29"
~~~

When available, run the existing QEMU module smoke test through the documented command.
Expected: no forbidden-pattern matches, existing smoke test remains valid, and no untracked
generated files remain except explicitly retained artifacts.

Step 4: Commit cleanup.

~~~sh
git add -A
git commit -m "chore: remove temporary S0 spike artifacts" \
  -m "Assisted-by: Codex: GPT-5"
~~~

## Final Acceptance Checklist

- [ ] Every manifest item 1.1-4.8 has a result and evidence.
- [ ] Group 1 has at least one working task lookup route.
- [ ] Group 2 2.4 matches proc/PID/maps in count, ranges, permissions and magic bytes.
- [ ] Group 3 3.2 or 3.3 reads the expected bytes.
- [ ] Group 4 confirms unavailable symbols and records wrapper scope.
- [ ] 4.8 records VM_MIXEDMAP and smaps before/after evidence.
- [ ] Safe probes have clean incremental dmesg; intentional unsafe probes are explicitly classified.
- [ ] No probe uses dynamic symbol lookup or mmap_sem.
- [ ] Results are recorded in docs/principles/08-kernel-module-limits.md with environment metadata.
- [ ] One-time code and generated artifacts are removed or clearly marked and ignored.
- [ ] Existing build, lint and QEMU smoke checks remain valid.

