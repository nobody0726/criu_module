#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"
cc -std=c11 -Wall -Wextra -Werror \
	-I"$root_dir/include" -I"$root_dir/userspace/criu-module-convert" \
	"$root_dir/tests/fixtures/a7-reader-harness.c" \
	"$root_dir/userspace/criu-module-convert/snapshot_reader.c" \
	-o "$tmp/a7-reader"

for name in a7-simple a7-session a7-pgid; do
	"$tmp/a7-reader" "$tmp/$name.bin"
done

set +e
"$tmp/a7-reader" "$tmp/a7-duplicate-pid.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a7-reader" "$tmp/a7-missing-parent.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a7-reader" "$tmp/a7-missing-leader.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a7-reader" "$tmp/a7-born-sid-conflict.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a7-reader" "$tmp/a7-owner-mismatch.bin"; rc=$?
test "$rc" -eq 4
"$tmp/a7-reader" "$tmp/a7-cross-namespace.bin"; rc=$?
test "$rc" -eq 1
set -e

echo 'A7_ABI_CONTRACT: PASS'
