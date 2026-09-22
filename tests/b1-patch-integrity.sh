#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PATCH="$ROOT/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"

python3 - "$PATCH" <<'PY'
import re
import sys
from pathlib import Path

patch_path = Path(sys.argv[1])
lines = patch_path.read_text().splitlines()

try:
    file_index = lines.index("--- a/kernel/criu_restore.c")
except ValueError:
    raise SystemExit("B1_PATCH_INTEGRITY: missing criu_restore.c file header")

hunk_index = next(
    (i for i in range(file_index + 1, len(lines))
     if re.match(r"^@@ -0,0 \+1,\d+ @@", lines[i])),
    None,
)
if hunk_index is None:
    raise SystemExit("B1_PATCH_INTEGRITY: missing criu_restore.c add hunk")

match = re.match(r"^@@ -0,0 \+1,(\d+) @@", lines[hunk_index])
declared = int(match.group(1))
end = next(
    (i for i in range(hunk_index + 1, len(lines))
     if lines[i].startswith(("@@ ", "--- "))),
    len(lines),
)
hunk_lines = [
    line for line in lines[hunk_index + 1:end]
    if line.startswith(("+", " ", "-"))
]
actual = len(hunk_lines)
if actual != declared:
    raise SystemExit(
        "B1_PATCH_INTEGRITY: criu_restore.c hunk declares "
        f"{declared} lines but contains {actual}"
    )

required_tail = [
    '+\treturn misc_register(&criu_restore_miscdev);',
    '+}',
    '+device_initcall(criu_restore_init);',
]
if hunk_lines[-3:] != required_tail:
    raise SystemExit(
        "B1_PATCH_INTEGRITY: criu_restore.c hunk does not contain "
        "the complete init tail"
    )
PY

echo "B1_PATCH_INTEGRITY: PASS"
