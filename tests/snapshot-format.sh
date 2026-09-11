#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
fixture_dir=$(mktemp -d)
trap 'rm -rf "$fixture_dir"' EXIT

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

def make(path, truncate=False, tamper=None):
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
    if tamper == 'payload':
        data = data[:HEADER + TLV] + b'X' + data[HEADER + TLV + 1:]
    elif tamper == 'header':
        data = bytes([data[0] ^ 1]) + data[1:]
    path.write_bytes(data[:-3] if truncate else data)
    return data

data = make(out / 'snapshot-minimal.bin')
make(out / 'snapshot-truncated.bin', truncate=True)
make(out / 'snapshot-tampered-payload.bin', tamper='payload')
make(out / 'snapshot-tampered-header.bin', tamper='header')

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
assert struct.unpack_from('<H', data, 14)[0] == 0
assert struct.unpack_from('<H', data, HEADER + 2)[0] == 0
assert struct.unpack_from('<I', data, HEADER + 4)[0] == 0
assert struct.unpack_from('<Q', data, 48)[0] == len(data)
assert frc == 2

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

def validate(path):
    blob = path.read_bytes()
    if len(blob) < HEADER + FOOTER:
        return False
    h = struct.unpack_from('<Q I H H I I I I Q I I Q Q', blob)
    if h[0] != MAGIC or h[1] != VERSION or h[2] != HEADER or h[3] != 0 or h[10] != 0:
        return False
    if h[11] != len(blob) or h[11] > 1024 * 1024 * 1024:
        return False
    footer = struct.unpack_from('<Q I I Q', blob, len(blob) - FOOTER)
    if footer[:3] != (MAGIC, VERSION, h[9]):
        return False
    off, count = HEADER, 0
    saw_end = False
    while off < len(blob) - FOOTER:
        if off + TLV > len(blob) - FOOTER:
            return False
        typ, flags, reserved, length = struct.unpack_from('<HHIQ', blob, off)
        if flags or reserved or length > MAX_RECORD:
            return False
        off += TLV
        if off + length > len(blob) - FOOTER:
            return False
        count += 1
        if count > MAX_RECORDS:
            return False
        if typ == 0xffff:
            saw_end = length == 0 and off == len(blob) - FOOTER
        off += length
    if not saw_end or count != h[9] or footer[3] != h[12]:
        return False
    return checksum(blob[:56] + b'\0' * 8 + blob[64:-FOOTER]) == h[12]

assert validate(out / 'snapshot-minimal.bin')
assert not validate(out / 'snapshot-truncated.bin')
assert not validate(out / 'snapshot-tampered-payload.bin')
assert not validate(out / 'snapshot-tampered-header.bin')

assert MAX_RECORDS > 0 and MAX_RECORD < 1 << 32
print('SNAPSHOT_FORMAT: PASS')
PY
