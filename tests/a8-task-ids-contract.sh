#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
converter="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a8-task-ids.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"

"$converter" "$tmp/a8-full-task-ids.bin" -D "$tmp/images" \
	>"$tmp/converter.log" 2>&1
test -s "$tmp/images/core-100.img"
test -s "$tmp/images/core-101.img"

python3 - "$tmp/images" <<'PY'
import pathlib
import struct
import sys

d = pathlib.Path(sys.argv[1])

def varint(blob, off):
    value = 0
    shift = 0
    while True:
        byte = blob[off]
        off += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, off
        shift += 7

def messages(path):
    blob = (d / path).read_bytes()
    off = 8
    out = []
    while off < len(blob):
        size = struct.unpack_from('<I', blob, off)[0]
        off += 4
        out.append(blob[off:off + size])
        off += size
    assert off == len(blob), path
    return out

def fields(message):
    out = {}
    off = 0
    while off < len(message):
        tag, off = varint(message, off)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, off = varint(message, off)
        elif wire == 2:
            size, off = varint(message, off)
            value = message[off:off + size]
            off += size
        else:
            raise AssertionError((number, wire))
        out.setdefault(number, []).append(value)
    return out

expected = {
    100: (501, 77, 100, 100),
    101: (502, 77, 101, 101),
}
for pid, ids_tuple in expected.items():
    core = fields(messages(f'core-{pid}.img')[0])
    ids = fields(core[4][0])
    actual = tuple(ids[i][0] for i in range(1, 5))
    assert actual == ids_tuple, (pid, actual, ids_tuple)
PY

set +e
"$converter" "$tmp/a8-missing-task-ids.bin" -D "$tmp/missing" \
	>"$tmp/missing.log" 2>&1
missing_rc=$?
"$converter" "$tmp/a8-nonthread-shared-vm.bin" -D "$tmp/shared-vm" \
	>"$tmp/shared-vm.log" 2>&1
shared_vm_rc=$?
set -e

test "$missing_rc" -eq 4
test "$shared_vm_rc" -eq 1

echo 'A8_TASK_IDS_CONTRACT: PASS'
