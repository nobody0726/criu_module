#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a8-shmem.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"

"$root_dir/userspace/criu-module-convert/criu-module-convert" \
	"$tmp/a8-shmem-shared.bin" -D "$tmp/images" \
	>"$tmp/converter.log" 2>&1

test -s "$tmp/images/mm-400.img"
test -s "$tmp/images/mm-401.img"
test -s "$tmp/images/pagemap-shmem-77.img"
test -s "$tmp/images/pages-100077.img"
test ! -e "$tmp/images/pagemap-shmem-78.img"

python3 - "$tmp/images" <<'PY'
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])

def varint(data, pos):
    value = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, pos
        shift += 7

def fields_from_message(message):
    pos = 0
    item = {}
    while pos < len(message):
        tag, pos = varint(message, pos)
        field = tag >> 3
        wire = tag & 7
        if wire == 0:
            value, pos = varint(message, pos)
            if field in item:
                item[field] = item[field] + [value] if isinstance(item[field], list) else [item[field], value]
            else:
                item[field] = value
        elif wire == 2:
            length, pos = varint(message, pos)
            value = message[pos:pos + length]
            if field in item:
                item[field] = item[field] + [value] if isinstance(item[field], list) else [item[field], value]
            else:
                item[field] = value
            pos += length
        else:
            raise AssertionError((field, wire))
    return item

def fields(path):
    blob = path.read_bytes()
    off = 8
    out = []
    while off < len(blob):
        size = struct.unpack_from("<I", blob, off)[0]
        off += 4
        out.append(fields_from_message(blob[off:off + size]))
        off += size
    return out

for pid, address in ((400, 0x500000), (401, 0x700000)):
    entries = fields(root / f"mm-{pid}.img")
    assert len(entries) == 1, (pid, entries)
    nested = entries[0][14]
    if not isinstance(nested, list):
        nested = [nested]
    vmas = []
    for payload in nested:
        vmas.append(fields_from_message(payload))
    assert len(vmas) == 1, (pid, vmas)
    vma = vmas[0]
    assert vma[1] == address, (pid, vma)
    assert vma[4] == 77, (pid, vma)
    assert vma[7] & (1 << 8), (pid, vma)

shmem = (root / "pages-100077.img").read_bytes()
assert shmem.count(bytes([0xA8]) * 8192) == 1
assert len(shmem) == 8192
PY

echo 'A8_SHMEM_CONTRACT: PASS'
