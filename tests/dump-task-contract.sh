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
grep -q 'thread.uw.tp_value' "$task_c"
grep -q 'task_pt_regs' "$task_c"
grep -q 'rcu_read_lock' "$task_c"
grep -q 'rcu_read_unlock' "$task_c"
grep -q 'CRIU_SNAPSHOT_REC_FD' "$files_h"
grep -q 'CRIU_SNAPSHOT_REC_FS' "$files_h"
grep -q 'S_ISREG' "$files_c"
grep -q 'walk_fds_prepared' "$files_c"
grep -q 'CRIU_SNAPSHOT_UNSUPPORTED' "$files_c"
grep -q 'checkpoint/dump_task.o' "$makefile"
grep -q 'checkpoint/dump_files.o' "$makefile"
# Linux 5.10.29 does not export the files_struct reference helpers to modules.
! grep -qE '\b(get_files_struct|put_files_struct)\s*\(' "$files_c"
grep -q 'task->files' "$files_c"

# Signal/timer handling remains A6; A5 expands the FD table beyond 0/1/2.
grep -q 'sighand' "$task_c"
grep -q 'timers' "$task_c"
grep -q 'atomic_inc(&files->count)' "$files_c"
grep -q 'files_fdtable' "$files_c"
grep -q 'get_file' "$files_c"
printf 'DUMP_TASK: PASS\n'
