#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
writer="$root_dir/kernel_module/checkpoint/snapshot_writer.c"
header="$root_dir/kernel_module/checkpoint/snapshot_writer.h"
makefile="$root_dir/kernel_module/Makefile"

test -s "$writer"
test -s "$header"
grep -q 'filp_open' "$writer"
grep -q 'kernel_write' "$writer"
grep -q 'CRIU_SNAPSHOT_MAX_RECORD_SIZE' "$writer"
grep -q 'CRIU_SNAPSHOT_MAX_TOTAL_SIZE' "$writer"
grep -q 'CRIU_SNAPSHOT_REC_END' "$writer"
grep -q 'CRIU_SNAPSHOT_FOOTER_SIZE' "$writer"
grep -q 'rename_atomic' "$writer"
grep -q 'unlink_path' "$writer"
grep -q 'vfs_fsync' "$writer"
grep -q 'snapshot_writer.o' "$makefile"

# Ensure the checksum is computed before publishing the final path and the
# failure path removes the temporary artifact.
python3 - "$writer" <<'PY'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
assert s.index('sha256_file') < s.index('rename_atomic(w->tmp_path, w->path)')
assert s.index('criu_snapshot_writer_abort') < len(s)
assert 'if (w->tmp_path) unlink_path(w->tmp_path);' in s
assert 'length > CRIU_SNAPSHOT_MAX_RECORD_SIZE' in s
assert 'w->total_size > CRIU_SNAPSHOT_MAX_TOTAL_SIZE' in s
print('SNAPSHOT_WRITER: PASS')
PY
