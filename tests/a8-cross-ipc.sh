#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
converter="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a8-cross-ipc.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
python3 "$root_dir/tests/fixtures/a7-snapshot-builder.py" "$tmp"

"$converter" "$tmp/a8-cross-pipe.bin" -D "$tmp/pipe-images" \
	>"$tmp/pipe.log" 2>&1
"$converter" "$tmp/a8-cross-unix.bin" -D "$tmp/unix-images" \
	>"$tmp/unix.log" 2>&1

test -s "$tmp/pipe-images/pipes-data.img"
test -s "$tmp/unix-images/unixsk.img"
test -s "$tmp/unix-images/sk-queues.img"

python3 - "$tmp" <<'PY'
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])

def count(path, header=8, raw=False):
    blob = path.read_bytes()
    off = header
    entries = 0

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

    while off < len(blob):
        size = struct.unpack_from("<I", blob, off)[0]
        off += 4
        message = blob[off:off + size]
        off += size
        if raw:
            payload_len = 0
            pos = 0
            while pos < size:
                tag, pos = varint(message, pos)
                if tag >> 3 == 2:
                    payload_len, pos = varint(message, pos)
                    break
                if tag & 7 == 0:
                    _, pos = varint(message, pos)
                else:
                    length, pos = varint(message, pos)
                    pos += length
            off += payload_len
        entries += 1
    assert off == len(blob), path
    return entries

assert count(root / "pipe-images" / "pipes-data.img", raw=True) == 1
assert count(root / "unix-images" / "unixsk.img") == 2
assert count(root / "unix-images" / "sk-queues.img", raw=True) == 2
PY

echo 'A8_CROSS_IPC: PASS'
