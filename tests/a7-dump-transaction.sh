#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
writer_c="$root_dir/kernel_module/checkpoint/snapshot_writer.c"
writer_h="$root_dir/kernel_module/checkpoint/snapshot_writer.h"
tree_c="$root_dir/kernel_module/checkpoint/dump_pstree.c"
dump_c="$root_dir/kernel_module/checkpoint/dump.c"
main_c="$root_dir/kernel_module/core/main.c"

for f in "$writer_c" "$writer_h" "$tree_c" "$dump_c" "$main_c"; do
	test -s "$f"
done

grep -q 'criu_snapshot_writer_process_record' "$writer_c" "$writer_h"
grep -q 'CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE' "$writer_c"
grep -q 'CRIU_SNAPSHOT_REC_PSTREE' "$tree_c"
grep -q 'sort(' "$tree_c"
grep -q 'CRIU_SNAPSHOT_F_PSTREE' "$dump_c"
grep -q 'criu_dump_process_tree' "$main_c" "$dump_c"
grep -q 'dump-tree' "$main_c"

python3 - "$writer_c" "$tree_c" <<'PY'
import pathlib
import sys

writer, tree = map(lambda p: pathlib.Path(p).read_text(), sys.argv[1:])
assert writer.index("criu_snapshot_writer_process_record") < writer.index("criu_snapshot_writer_finish")
assert tree.index("criu_freeze_process_count") < tree.index("criu_snapshot_writer_record")
assert "criu_snapshot_writer_process_record" not in tree
print("A7_DUMP_TRANSACTION: PASS")
PY
