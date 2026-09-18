#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$root_dir/tests/fixtures/a6-snapshot-builder.py" "$tmp"
make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null

"$bin" "$tmp/a6-valid.bin" -D "$tmp/valid-images"

set +e
"$bin" "$tmp/a6-missing.bin" -D "$tmp/missing-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-duplicate.bin" -D "$tmp/duplicate-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-queue-gap.bin" -D "$tmp/gap-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-bad-siginfo.bin" -D "$tmp/bad-siginfo-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-unknown-header-flag.bin" -D "$tmp/unknown-header-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-record-without-flag.bin" -D "$tmp/no-flag-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-unknown-mandatory.bin" -D "$tmp/unknown-images"; rc=$?
test "$rc" -eq 1
set -e

test ! -e "$tmp/missing-images"
test ! -e "$tmp/duplicate-images"
test ! -e "$tmp/gap-images"
test ! -e "$tmp/bad-siginfo-images"
test ! -e "$tmp/unknown-header-images"
test ! -e "$tmp/no-flag-images"
test ! -e "$tmp/unknown-images"
echo 'A6_ABI_CONTRACT: PASS'
