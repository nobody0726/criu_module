#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
model="$root_dir/userspace/criu-module-convert/criu_model.c"
reader="$root_dir/userspace/criu-module-convert/snapshot_reader.c"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a7-converter.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

grep -q 'struct process_model' "$model"
grep -q 'process_model_touch' "$model"
grep -q 'validate_a7_indexed_model' "$model"
grep -q 'pstree_count' "$model"
grep -q 'CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE' "$model"
grep -q 'pstree_messages' "$model"
grep -q 'mkdtemp' "$root_dir/userspace/criu-module-convert/criu_model.c"
grep -q 'CRIU_SNAPSHOT_F_PSTREE' "$reader"

make -C "$root_dir/userspace/criu-module-convert" clean \
	all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"
set +e
"$root_dir/userspace/criu-module-convert/criu-module-convert" \
	"$tmp/a7-full-multi.bin" -D "$tmp/images" >"$tmp/converter.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
	cat "$tmp/converter.log" >&2
	echo 'converter rejected valid A7 multi-process image emission' >&2
	exit 1
fi
for file in \
	inventory.img pstree.img files.img reg-files.img \
	core-100.img core-101.img mm-100.img mm-101.img \
	pagemap-100.img pagemap-101.img ids-100.img ids-101.img \
	fs-100.img fs-101.img creds-100.img creds-101.img \
	fdinfo-100.img fdinfo-101.img; do
	test -s "$tmp/images/$file"
done
test -e "$tmp/images/pages-1.img"
test -e "$tmp/images/pages-2.img"
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

pm100 = [fields(m) for m in messages('pagemap-100.img')]
pm101 = [fields(m) for m in messages('pagemap-101.img')]
assert pm100[0][1][0] == 1, pm100
assert pm101[0][1][0] == 2, pm101
assert pm100[1][2][0] == 1, pm100
assert pm101[1][2][0] == 1, pm101
pages1 = (d / 'pages-1.img').read_bytes()
pages2 = (d / 'pages-2.img').read_bytes()
assert len(pages1) == 4096, len(pages1)
assert len(pages2) == 4096, len(pages2)
assert pages1 == bytes([100]) * 4096
assert pages2 == bytes([101]) * 4096

for pid in (100, 101):
    core = fields(messages(f'core-{pid}.img')[0])
    ids = fields(core[4][0])
    assert ids[1][0] == pid, ids
    assert ids[2][0] == pid, ids
    assert ids[3][0] == pid, ids
    assert ids[4][0] == pid, ids
PY
echo 'A7_CONVERTER_IMAGES: PASS'
