#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)

test -f "$root_dir/kernel_module/checkpoint/dump_shared.h"
test -f "$root_dir/kernel_module/checkpoint/dump_shared.c"

grep -q 'checkpoint/dump_shared.o' "$root_dir/kernel_module/Makefile"

grep -q 'struct criu_dump_shared_ctx' \
	"$root_dir/kernel_module/checkpoint/dump_shared.h"
grep -q 'criu_dump_shared_ctx_init' \
	"$root_dir/kernel_module/checkpoint/dump_shared.h"
grep -q 'criu_dump_shared_ctx_destroy' \
	"$root_dir/kernel_module/checkpoint/dump_shared.h"

if grep -q 'criu_objmap_new' "$root_dir/kernel_module/checkpoint/dump_files.c"; then
	echo 'dump_files.c still allocates per-process objmaps' >&2
	exit 1
fi
if grep -q 'criu_objmap_free' "$root_dir/kernel_module/checkpoint/dump_files.c"; then
	echo 'dump_files.c still frees per-process objmaps' >&2
	exit 1
fi

grep -q 'criu_dump_shared_ctx shared_ctx' \
	"$root_dir/kernel_module/checkpoint/dump.c"
grep -q 'criu_dump_shared_ctx_init(&shared_ctx)' \
	"$root_dir/kernel_module/checkpoint/dump.c"
grep -q 'criu_dump_shared_ctx_destroy(&shared_ctx)' \
	"$root_dir/kernel_module/checkpoint/dump.c"
grep -q 'criu_dump_files_process(.*&shared_ctx' \
	"$root_dir/kernel_module/checkpoint/dump.c"

echo 'A8_OBJMAP_SCOPE_CONTRACT: PASS'
