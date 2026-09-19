#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
converter="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a8-shared-fdtable.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

dump_c="$root_dir/kernel_module/checkpoint/dump.c"
dump_files_c="$root_dir/kernel_module/checkpoint/dump_files.c"
test "$(grep -c 'right_files == left_files' "$dump_c" || true)" -eq 0
grep -q 'frozen_owners += owner_tasks' "$dump_files_c"
grep -q 'criu_freeze_process_count' "$dump_files_c"

make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"

"$converter" "$tmp/a8-shared-files.bin" -D "$tmp/images" \
	>"$tmp/converter.log" 2>&1

test -s "$tmp/images/fdinfo-77.img"
test ! -e "$tmp/images/fdinfo-400.img"
test ! -e "$tmp/images/fdinfo-401.img"

python3 - "$tmp/images" <<'PY'
import pathlib
import struct
import sys

d = pathlib.Path(sys.argv[1])

def messages(path):
    blob = (d / path).read_bytes()
    off = 8
    out = []
    while off < len(blob):
        size = struct.unpack_from("<I", blob, off)[0]
        off += 4
        out.append(blob[off:off + size])
        off += size
    assert off == len(blob), path
    return out

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

fdinfo = messages("fdinfo-77.img")
assert len(fdinfo) == 1, len(fdinfo)
assert fields(fdinfo[0])[4][0] == 0
PY

"$converter" "$tmp/a8-shared-file-object.bin" -D "$tmp/object-images" \
	>"$tmp/object-converter.log" 2>&1

python3 - "$tmp/object-images" <<'PY'
import pathlib
import struct
import sys

d = pathlib.Path(sys.argv[1])

def messages(path):
    blob = (d / path).read_bytes()
    off = 8
    out = []
    while off < len(blob):
        size = struct.unpack_from("<I", blob, off)[0]
        off += 4
        out.append(blob[off:off + size])
        off += size
    return out

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

left = fields(messages("fdinfo-77.img")[0])
right = fields(messages("fdinfo-78.img")[0])
assert left[1][0] == right[1][0], (left, right)
PY

echo 'A8_SHARED_FDTABLE: PASS'
