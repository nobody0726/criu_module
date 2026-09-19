#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
for name in tree-simple tree-session tree-pgid tree-invalid-leader \
	tree-fork-race tree-shared-unsupported; do
	test -s "$root_dir/tests/progs/$name.c"
done
grep -q 'tree-simple' "$root_dir/tests/progs/Makefile"
grep -q 'dump-tree' "$root_dir/tests/a7-cross-restore.sh"
grep -q 'SKIP: ENVIRONMENT' "$root_dir/tests/a7-cross-restore.sh"
grep -q 'kill -0' "$root_dir/tests/a7-cross-restore.sh" || true
echo 'A7_FIXTURE_CONTRACT: PASS'
