#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
writer="$root_dir/kernel_module/checkpoint/snapshot_writer.c"
dump="$root_dir/kernel_module/checkpoint/dump.c"

for f in "$writer" "$dump" \
	"$root_dir/kernel_module/checkpoint/dump_task.c" \
	"$root_dir/kernel_module/checkpoint/dump_threads.c" \
	"$root_dir/kernel_module/checkpoint/dump_mm.c" \
	"$root_dir/kernel_module/checkpoint/dump_files.c"; do
	test -s "$f"
done

grep -q 'process_owner_pid' "$writer"
grep -q 'criu_snapshot_writer_set_process_owner' "$writer"
grep -q 'criu_dump_process_tree' "$dump"
grep -q 'criu_snapshot_writer_set_process_owner(&writer, view.pid)' "$dump"
grep -q 'criu_dump_threads(view.leader' "$dump"
grep -q 'criu_dump_mm(view.leader' "$dump"
grep -q 'criu_dump_files(view.leader' "$dump"

echo 'A7_COLLECTOR_CONTRACT: PASS'
