#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
tree_c="$root_dir/kernel_module/checkpoint/collect_tree.c"
tree_h="$root_dir/kernel_module/checkpoint/collect_tree.h"
freeze_c="$root_dir/kernel_module/checkpoint/freeze.c"
kernel_h="$root_dir/kernel_module/include/criu_kernel.h"
makefile="$root_dir/kernel_module/Makefile"

for f in "$tree_c" "$tree_h" "$freeze_c" "$kernel_h" "$makefile"; do
	test -s "$f"
done

grep -q 'CRIU_A7_MAX_PROCESSES' "$tree_h"
grep -q 'struct task_struct \*\*stack' "$tree_c"
grep -q 'task_active_pid_ns' "$tree_c"
grep -q 'get_pid_ns' "$tree_c"
grep -q 'put_pid_ns' "$tree_c"
grep -q 'criu_freeze_process_count' "$kernel_h"
grep -q 'criu_freeze_process_get' "$kernel_h"
grep -q 'criu_freeze_process_task_count' "$kernel_h"
grep -q 'criu_freeze_process_task_get' "$kernel_h"
grep -q 'criu_cgroup_freeze_process_set' "$freeze_c"
grep -q 'criu_collect_tree' "$freeze_c"
grep -q 'checkpoint/collect_tree.o' "$makefile"

if rg -n 'criu_target_get|for_each_process|criu_snapshot_writer_record' \
	"$tree_c"; then
	echo 'A7 tree collector must consume explicit task relationships only' >&2
	exit 1
fi
if rg -n 'criu_snapshot_writer_record|kernel_write' "$tree_c" "$freeze_c"; then
	echo 'tree discovery must not perform snapshot I/O' >&2
	exit 1
fi
if rg -n 'criu_collect_tree.*criu_collect_tree' "$tree_c"; then
	echo 'tree discovery must be iterative, not recursive' >&2
	exit 1
fi

echo 'A7_TREE_CONTRACT: PASS'
