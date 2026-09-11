#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
task_c="$root_dir/kernel_module/checkpoint/dump_task.c"
task_h="$root_dir/kernel_module/checkpoint/dump_task.h"
files_c="$root_dir/kernel_module/checkpoint/dump_files.c"
files_h="$root_dir/kernel_module/checkpoint/dump_files.h"
makefile="$root_dir/kernel_module/Makefile"

for f in "$task_c" "$task_h" "$files_c" "$files_h"; do test -s "$f"; done
grep -q 'CRIU_SNAPSHOT_REC_TASK' "$task_h"
grep -q 'CRIU_SNAPSHOT_REC_REGS' "$task_h"
grep -q 'CRIU_SNAPSHOT_REC_CREDS' "$task_h"
grep -q 'CRIU_SNAPSHOT_UNSUPPORTED' "$task_c"
grep -q 'signal_pending' "$task_c"
grep -q 'rlim' "$task_c"
grep -q 'task_pt_regs' "$task_c"
grep -q 'CRIU_SNAPSHOT_REC_FD' "$files_h"
grep -q 'CRIU_SNAPSHOT_REC_FS' "$files_h"
grep -q 'S_ISREG' "$files_c"
grep -q 'fd 0' "$files_c"
grep -q 'CRIU_SNAPSHOT_UNSUPPORTED' "$files_c"
grep -q 'checkpoint/dump_task.o' "$makefile"
grep -q 'checkpoint/dump_files.o' "$makefile"

# Keep the A3 boundary explicit: no signal handlers, pending signals, timers,
# sockets, or descriptors beyond stdin/stdout/stderr.
grep -q 'sighand' "$task_c"
grep -q 'timers' "$task_c"
grep -q 'get_files_struct' "$files_c"
grep -q 'files_fdtable' "$files_c"
grep -q 'fd > 2' "$files_c"
printf 'DUMP_TASK: PASS\n'
