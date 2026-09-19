#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$root_dir/tests/fixtures/a6-snapshot-builder.py" "$tmp"

set +e
"$bin" "$tmp/a6-missing.bin" -D "$tmp/missing"; missing_rc=$?
"$bin" "$tmp/a6-duplicate.bin" -D "$tmp/duplicate"; duplicate_rc=$?
"$bin" "$tmp/a6-bad-notify-tid.bin" -D "$tmp/notify"; notify_rc=$?
set -e

test "$missing_rc" -eq 4
test "$duplicate_rc" -eq 4
test "$notify_rc" -eq 4
test ! -e "$tmp/missing"
test ! -e "$tmp/duplicate"
test ! -e "$tmp/notify"
echo 'A6_UNSUPPORTED: PASS'
