#!/usr/bin/env bash
# Contract for the B1 restore UAPI.  This is intentionally header-focused:
# the kernel implementation and real validator are introduced by later B1
# tasks, but their ABI must be fixed before code starts depending on it.
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
header="$root_dir/include/criu_restore_abi.h"
builder="$root_dir/tests/fixtures/b1-restore-plan-builder.py"
tmp="${TMPDIR:-/tmp}/b1-restore-abi.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

if [ ! -f "$header" ]; then
	echo "missing restore ABI header: $header" >&2
	exit 1
fi

python3 "$builder" valid > "$tmp/valid.json"
python3 - "$tmp/valid.json" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["errors"] == [], data["errors"]
assert data["plan"]["target_pid"] == 4242
assert data["plan"]["vmas_user_ptr"] != 0
PY

for case in bad-version bad-size zero-target-pid too-many-vmas unaligned duplicate-target overflow bad-kind; do
	python3 "$builder" "$case" > "$tmp/$case.json"
	python3 - "$tmp/$case.json" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["errors"], data
PY
done

cat > "$tmp/abi_check.c" <<'C'
#include <stddef.h>
#include <stdint.h>
#include "criu_restore_abi.h"

_Static_assert(CRIU_RESTORE_ABI_VERSION == 1U, "ABI version");
_Static_assert(CRIU_RESTORE_MAX_VMAS == 4096U, "max VMAs");
_Static_assert(CRIU_RESTORE_VALIDATE_V1 != CRIU_RESTORE_COMMIT_V1, "ioctls must differ");
_Static_assert(CRIU_RESTORE_VALIDATE_V1_SIZE == sizeof(struct criu_restore_plan_v1), "plan size macro");
_Static_assert(CRIU_RESTORE_VMA_RECORD_SIZE == sizeof(struct criu_restore_vma_v1), "VMA size macro");
_Static_assert(sizeof(struct criu_restore_vma_v1) == 40U, "VMA size");
_Static_assert(sizeof(struct criu_restore_plan_v1) == 104U, "plan size");
_Static_assert(offsetof(struct criu_restore_plan_v1, target_pid) == 16U, "target_pid offset");
_Static_assert(offsetof(struct criu_restore_plan_v1, vmas_user_ptr) == 24U, "vmas_user_ptr offset");
_Static_assert(offsetof(struct criu_restore_plan_v1, tls) == 96U, "tls offset");
_Static_assert(sizeof(((struct criu_restore_plan_v1 *)0)->vmas_user_ptr) == 8U, "pointer-width integer");
_Static_assert(sizeof(((struct criu_restore_plan_v1 *)0)->target_pid) == 4U, "target pid width");
_Static_assert(CRIU_RESTORE_VMA_ANON_PRIVATE == 1U, "anon private kind");
_Static_assert(CRIU_RESTORE_VMA_FILE_PRIVATE == 2U, "file private kind");
_Static_assert(CRIU_RESTORE_VMA_STACK == 3U, "stack kind");
_Static_assert(CRIU_RESTORE_VMA_VDSO == 4U, "vdso kind");

int main(void) { return 0; }
C

cc -std=c11 -Wall -Wextra -Werror -I"$root_dir/include" "$tmp/abi_check.c" -o "$tmp/abi_check"
"$tmp/abi_check"

python3 - "$header" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
assert "target_pid" in text
assert "vmas_user_ptr" in text
assert "VALIDATE only" in text
assert "COMMIT must not dereference" in text
assert not re.search(r'char\s+\*|\w+\s+\*\s+\w+;', text), "ABI must not embed C pointers"
for forbidden in ("PATH_MAX", "pb-c"):
    assert forbidden not in text
PY

echo "B1_RESTORE_ABI_CONTRACT: PASS"
