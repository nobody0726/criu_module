#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

grep -q 'CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE' "$ROOT/include/criu_snapshot.h"
grep -q 'object_id' "$ROOT/include/criu_snapshot.h"
grep -q 'CRIU_FD_TYPE_PIPE' "$ROOT/include/criu_snapshot.h"
grep -q 'criu_objmap_get' "$ROOT/kernel_module/core/objmap.h"
grep -q 'criu_walk_fds' "$ROOT/kernel_module/checkpoint/dump_files.h"
grep -q 'O_CREAT.*O_EXCL.*O_TRUNC' "$ROOT/kernel_module/checkpoint/dump_files.c"
grep -q 'CRIU_FD_TYPE_UNIX' "$ROOT/kernel_module/checkpoint/dump_files.c"
grep -q 'CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE' \
	"$ROOT/userspace/criu-module-convert/criu_model.c"
grep -q 'object_id' "$ROOT/userspace/criu-module-convert/criu_model.c"

# The fd snapshot callback is allowed to run after lock release only. Keep
# this structural guard close to the implementation so a future refactor
# cannot accidentally perform path I/O under file_lock/RCU.
awk '
  /spin_lock\(&files->file_lock\)/ { locked = 1 }
  /spin_unlock\(&files->file_lock\)/ { locked = 0 }
  locked && /(d_path|kernel_read|criu_snapshot_writer_record)/ { bad = 1 }
  END { exit bad }
' "$ROOT/kernel_module/checkpoint/dump_files.c"

echo 'A5_FD_CONTRACT: PASS'
