#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b2-pstree.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
common = 0x54564319
magic = 0x50273030

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

records = [
    entry(1000, 0, 1000, 1000),
    entry(1001, 1000, 1001, 1001),
    entry(1002, 1001, 1000, 1000),
    entry(1003, 1000, 1000, 1000),
]
payload = struct.pack("<II", common, magic)
for record in records:
    payload += struct.pack("<I", len(record)) + record
(out / "pstree.img").write_bytes(payload)
PY

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
cc -O2 -Wall -Wextra -Werror -std=c11 -I"$ROOT/include" \
	-I"$ROOT/userspace/mini-restore" \
	"$ROOT/tests/progs/b2-pstree-probe.c" \
	"$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/probe"

"$TMP/probe" "$TMP" >"$TMP/out"
grep -Fq 'root=1000 count=4' "$TMP/out"
grep -Fq 'item=1001 ppid=1000 sid=1001 pgid=1001 born_sid=1000 before_setsid=0' "$TMP/out"
grep -Fq 'item=1002 ppid=1001 sid=1000 pgid=1000 born_sid=-1 before_setsid=1' "$TMP/out"

python3 - "$TMP" <<'PY'
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "pstree.img"
data = bytearray(path.read_bytes())
data[-1] = 0
path.write_bytes(data)
PY
if "$TMP/probe" "$TMP" >/dev/null 2>&1; then
	echo "malformed pstree unexpectedly passed" >&2
	exit 1
fi

echo "B2_PSTREE_CONTRACT: PASS"
