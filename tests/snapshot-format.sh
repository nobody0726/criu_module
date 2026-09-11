#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
fixture_dir="$root_dir/tests/fixtures"
mkdir -p "$fixture_dir"

python3 - "$root_dir" "$fixture_dir" <<'PY'
import hashlib
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
MAGIC = 0x43524955534e5033
VERSION = 1
HEADER = 64
TLV = 16
FOOTER = 24
MAX_RECORDS = 65536
MAX_RECORD = 64 * 1024 * 1024

def checksum(data):
    return int.from_bytes(hashlib.sha256(data).digest()[:8], 'little')

def make(path, truncate=False):
    records = []
    payload = b'a3-fixture'
    records.append(struct.pack('<HHIQ', 1, 0, 0, len(payload)) + payload)
    records.append(struct.pack('<HHIQ', 0xffff, 0, 0, 0))
    body = b''.join(records)
    total = HEADER + len(body) + FOOTER
    head0 = struct.pack('<Q I H H I I I I Q I I Q Q', MAGIC, VERSION, HEADER, 0,
                        0x3e, 4096, 1234, 1234, 7, 2, 0, total, 0)
    digest = checksum(head0 + body)
    head = struct.pack('<Q I H H I I I I Q I I Q Q', MAGIC, VERSION, HEADER, 0,
                       0x3e, 4096, 1234, 1234, 7, 2, 0, total, digest)
    footer = struct.pack('<Q I I Q', MAGIC, VERSION, 2, digest)
    data = head + body + footer
    path.write_bytes(data[:-3] if truncate else data)
    return data

data = make(out / 'snapshot-minimal.bin')
make(out / 'snapshot-truncated.bin', truncate=True)

assert len(data) == HEADER + (TLV + 10) + TLV + FOOTER
magic, version, hsize = struct.unpack_from('<Q I H', data)
assert magic == MAGIC and version == VERSION and hsize == HEADER
typ, flags, reserved, length = struct.unpack_from('<HHIQ', data, HEADER)
assert typ == 1 and flags == 0 and reserved == 0 and length == 10
end_off = HEADER + TLV + length
end_type, _, _, end_len = struct.unpack_from('<HHIQ', data, end_off)
assert end_type == 0xffff and end_len == 0
fm, fv, frc, fsum = struct.unpack_from('<QIIQ', data, len(data) - FOOTER)
assert (fm, fv, frc, fsum) == (MAGIC, VERSION, 2, struct.unpack_from('<Q', data, 56)[0])
assert checksum(data[:56] + b'\0' * 8 + data[64:-FOOTER]) == fsum

truncated = (out / 'snapshot-truncated.bin').read_bytes()
assert len(truncated) < len(data)
try:
    if len(truncated) < HEADER + FOOTER:
        raise ValueError
    _, _, _, _, length = struct.unpack_from('<HHIQ', truncated, HEADER)
    if HEADER + TLV + length + TLV + FOOTER > len(truncated):
        raise ValueError
    raise AssertionError('truncated fixture accepted')
except ValueError:
    pass

assert MAX_RECORDS > 0 and MAX_RECORD < 1 << 32
print('SNAPSHOT_FORMAT: PASS')
PY
