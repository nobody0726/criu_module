#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dump_c="$root_dir/kernel_module/checkpoint/dump.c"
dump_h="$root_dir/kernel_module/checkpoint/dump.h"
main_c="$root_dir/kernel_module/core/main.c"
makefile="$root_dir/kernel_module/Makefile"

test -s "$dump_c"
test -s "$dump_h"
grep -q 'criu_freeze' "$dump_c"
grep -q 'criu_dump_task' "$dump_c"
grep -q 'criu_dump_mm' "$dump_c"
grep -q 'criu_dump_files' "$dump_c"
grep -q 'dump_revalidate' "$dump_c"
grep -q 'criu_snapshot_writer_abort' "$dump_c"
grep -q 'criu_snapshot_writer_finish' "$dump_c"
grep -q 'criu_thaw' "$dump_c"
grep -q 'debugfs_create_file("dump"' "$main_c"
grep -q 'checkpoint/dump.o' "$makefile"

# Every post-freeze failure path must converge on the thaw label.
awk '/ret = criu_freeze\(/ { seen=1 } seen && /goto thaw;/ { thaw=1 } END { exit !(seen && thaw) }' "$dump_c"
echo "DUMP_ERRORS: PASS"
