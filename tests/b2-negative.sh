#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b2-negative.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
def varint(value):
    data = bytearray()
    while value >= 0x80:
        data.append((value & 0x7f) | 0x80)
        value >>= 7
    data.append(value)
    return bytes(data)
def field(number, value):
    return varint(number << 3) + varint(value)
def entry(pid, ppid, pgid, sid):
    return b"".join((field(1, pid), field(2, ppid), field(3, pgid),
                     field(4, sid), field(5, pid)))

# Duplicate root PID and a missing session leader are both rejected before
# any task image is opened or any carrier is created.
payload = struct.pack("<II", 0x54564319, 0x50273030)
for item in (entry(1000, 0, 1000, 1000),
             entry(1000, 0, 1000, 1000)):
    payload += struct.pack("<I", len(item)) + item
(out / "pstree.img").write_bytes(payload)
PY

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
if "$ROOT/userspace/mini-restore/mini-restore" \
	--pstree "$TMP" --dry-run >"$TMP/out" 2>&1; then
	echo "invalid pstree unexpectedly passed" >&2
	exit 1
fi
grep -Eq 'FORMAT|UNSUPPORTED' "$TMP/out"

cc -O2 -Wall -Wextra -Werror -std=c11 \
	-I"$ROOT/include" -I"$ROOT/userspace/mini-restore" \
	"$ROOT/tests/progs/b2-cleanup-probe.c" \
	"$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/cleanup"
"$TMP/cleanup" | grep -Fq cleanup-ok
echo "B2_NEGATIVE: PASS"
