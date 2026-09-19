#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILDER="$ROOT/tests/fixtures/b1-image-builder.py"
BIN="$ROOT/userspace/mini-restore/mini-restore"
TMP="${TMPDIR:-/tmp}/b1-negative.$$"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null

expect_fail() {
	case_name=$1
	expected=$2
	python3 "$BUILDER" "$case_name" "$TMP/$case_name"
	if "$BIN" --images "$TMP/$case_name" --dry-run >"$TMP/$case_name.out" 2>&1; then
		echo "$case_name unexpectedly passed" >&2
		exit 1
	fi
	grep -Fq "$expected" "$TMP/$case_name.out" || {
		echo "$case_name missing diagnostic $expected" >&2
		cat "$TMP/$case_name.out" >&2
		exit 1
	}
}

expect_fail dirty-file-private UNSUPPORTED
expect_fail vdso-reloc UNSUPPORTED
expect_fail shared-mapping UNSUPPORTED
expect_fail threads UNSUPPORTED
expect_fail children UNSUPPORTED
expect_fail namespaces UNSUPPORTED
expect_fail unstable-file IO
expect_fail overlap FORMAT
expect_fail bad-field FORMAT

# Target-PID occupied and duplicate-COMMIT are kernel/live-device cases.  Until
# Task 10 connects real CRIU protobuf parsing and the guest gate, keep them as
# explicit non-PASS coverage rather than pretending a userspace dry-run can
# exercise COMMIT.
echo "B1_NEGATIVE: DEFER target-pid-occupied duplicate-commit live-kernel-only"
echo "B1_NEGATIVE: PASS"
