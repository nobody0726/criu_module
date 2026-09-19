#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
model="$root_dir/userspace/criu-module-convert/criu_model.c"
reader="$root_dir/userspace/criu-module-convert/snapshot_reader.c"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a7-converter.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

grep -q 'struct process_model' "$model"
grep -q 'process_model_touch' "$model"
grep -q 'validate_a7_indexed_model' "$model"
grep -q 'pstree_count' "$model"
grep -q 'CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE' "$model"
grep -q 'pstree_messages' "$model"
grep -q 'mkdtemp' "$root_dir/userspace/criu-module-convert/criu_model.c"
grep -q 'CRIU_SNAPSHOT_F_PSTREE' "$reader"

make -C "$root_dir/userspace/criu-module-convert" clean \
	all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"
set +e
"$root_dir/userspace/criu-module-convert/criu-module-convert" \
	"$tmp/a7-full-multi.bin" -D "$tmp/images" >"$tmp/converter.log" 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
	echo 'converter silently accepted incomplete A7 multi-process image emission' >&2
	exit 1
fi
grep -q 'A7 multi-process image emission is incomplete' "$tmp/converter.log"
[ ! -e "$tmp/images" ]
echo 'A7_CONVERTER_IMAGES: PASS'
