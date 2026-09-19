#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
converter="$root_dir/userspace/criu-module-convert/criu-module-convert"
builder="$root_dir/tests/fixtures/a6-snapshot-builder.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

make -C "$root_dir/userspace/criu-module-convert" clean all LDFLAGS= >/dev/null
python3 "$builder" "$tmp"
cp "$tmp/a6-valid.bin" "$tmp/snapshot.bin"
"$converter" "$tmp/snapshot.bin" -D "$tmp/images" >/dev/null
test -s "$tmp/images/core-1234.img"

python3 - "$tmp/images/core-1234.img" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
message = blob[12:12 + struct.unpack_from("<I", blob, 8)[0]]

def varint(data, offset):
    value = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, offset
        shift += 7

def fields(data):
    result = {}
    offset = 0
    while offset < len(data):
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire == 2:
            size, offset = varint(data, offset)
            value = data[offset:offset + size]
            offset += size
        else:
            raise AssertionError((number, wire))
        result.setdefault(number, []).append(value)
    return result

core = fields(message)
task = fields(core[3][0])
timers = fields(task[7][0])
assert len(task[15]) == 62
shared = fields(task[10][0])
assert len(shared[1]) == 1
thread_core = fields(core[5][0])
private = fields(thread_core[9][0])
assert len(private[1]) == 1
assert len(timers[1]) == len(timers[2]) == len(timers[3]) == 1
posix = fields(timers[4][0])
assert posix[1][0] == 1
assert posix[6][0] == 2
assert posix[7][0] == 0
print("A6 converter protobuf fields: PASS")
PY

echo 'A6_CONVERTER_IMAGES: PASS'
