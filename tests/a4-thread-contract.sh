#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
abi_h="$root_dir/include/criu_snapshot.h"
kernel_h="$root_dir/kernel_module/checkpoint/dump_threads.h"
kernel_c="$root_dir/kernel_module/checkpoint/dump_threads.c"
dump_c="$root_dir/kernel_module/checkpoint/dump.c"
model_c="$root_dir/userspace/criu-module-convert/criu_model.c"
reader_c="$root_dir/userspace/criu-module-convert/snapshot_reader.c"
makefile="$root_dir/kernel_module/Makefile"

for f in "$abi_h" "$kernel_h" "$kernel_c" "$dump_c" "$model_c" "$reader_c" "$makefile"; do
	test -s "$f"
done

grep -q 'CRIU_SNAPSHOT_REC_THREAD' "$abi_h"
grep -q 'struct criu_thread_record' "$kernel_h"
grep -q 'criu_dump_threads' "$dump_c"
grep -q 'get_task_struct' "$kernel_c"
grep -q 'put_task_struct' "$kernel_c"
grep -q 'rcu_read_lock' "$kernel_c"
grep -q 'rcu_read_unlock' "$kernel_c"
if grep -q 'kernel_write' "$kernel_c"; then
	echo 'thread enumeration must not perform image I/O' >&2
	exit 1
fi
grep -q 'CRIU_SNAPSHOT_REC_THREAD' "$reader_c"
grep -q 'threads' "$model_c"
grep -q 'core-%u.img' "$model_c"
grep -q 'dump_threads.o' "$makefile"

python3 - "$kernel_c" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
assert source.index('rcu_read_unlock') < source.index('criu_snapshot_writer_record')
assert source.index('get_task_struct') < source.index('rcu_read_unlock')
assert source.index('put_task_struct') > source.index('criu_snapshot_writer_record')
print('A4_THREAD_CONTRACT: PASS')
PY
