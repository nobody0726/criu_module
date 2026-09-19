#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/a8-abi-check.c" <<'C'
#include "criu_snapshot.h"

_Static_assert(CRIU_SNAPSHOT_REC_TASK_IDS == 20, "task ids record type");
_Static_assert(CRIU_SNAPSHOT_REC_SHMEM_OBJECT == 21, "shmem object record type");
_Static_assert(CRIU_SNAPSHOT_REC_SHMEM_PAGE_RUN == 22, "shmem page run record type");
_Static_assert(CRIU_SNAPSHOT_TASK_IDS_VERSION == 1, "task ids version");
_Static_assert(CRIU_SNAPSHOT_TASK_IDS_RECORD_SIZE == 32, "task ids record size constant");
_Static_assert(sizeof(struct criu_snapshot_task_ids_record) == 32,
	       "task ids record ABI size");

int main(void) { return 0; }
C

cc -std=c11 -Wall -Wextra -Werror -I"$root_dir/include" \
	"$tmp/a8-abi-check.c" -o "$tmp/a8-abi-check"
"$tmp/a8-abi-check"

python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"
cc -std=c11 -Wall -Wextra -Werror \
	-I"$root_dir/include" -I"$root_dir/userspace/criu-module-convert" \
	"$root_dir/tests/fixtures/a7-reader-harness.c" \
	"$root_dir/userspace/criu-module-convert/snapshot_reader.c" \
	-o "$tmp/a8-reader"

"$tmp/a8-reader" "$tmp/a8-valid-shared-files.bin"

set +e
"$tmp/a8-reader" "$tmp/a8-duplicate-task-ids.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a8-reader" "$tmp/a8-missing-task-ids.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a8-reader" "$tmp/a8-nonthread-shared-vm.bin"; rc=$?
test "$rc" -eq 1
set -e

echo 'A8_ABI_CONTRACT: PASS'
