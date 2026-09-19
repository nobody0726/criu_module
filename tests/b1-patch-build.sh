#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PATCH="$ROOT/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"
KDIR="${KDIR:-/Users/yhome/workspace/source_code/linux-5.10.29}"

test -s "$PATCH"
sh "$ROOT/tests/b1-kernel-patch-contract.sh"
sh "$ROOT/tests/b1-validate-contract.sh"
sh "$ROOT/tests/b1-vma-commit-contract.sh"

if [ -d "$KDIR" ]; then
	patch --dry-run -d "$KDIR" -p1 < "$PATCH" >/dev/null
	echo "B1_PATCH_BUILD: PATCH_DRY_RUN_OK"
else
	echo "B1_PATCH_BUILD: SKIP_DRY_RUN missing kernel tree: $KDIR"
fi

echo "B1_PATCH_BUILD: PASS"
