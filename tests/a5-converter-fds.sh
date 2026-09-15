#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

make -C "$ROOT/userspace/criu-module-convert" clean all LDFLAGS= >/dev/null
python3 - "$TMP/valid.bin" "$TMP/pipe.bin" <<'PY'
import hashlib, struct, sys

valid, pipe = sys.argv[1:]
MAGIC = 0x43524955534e5033
PAGE = 4096

def path(s):
    b = s.encode()
    return b + b'\0' * (512 - len(b))

def tlv(kind, payload):
    return struct.pack('<HHIQ', kind, 0, 0, len(payload)) + payload

def fd(n, obj, flags=0x202, kind=1):
    return struct.pack('<2I5Q512sQ2I', n, 0o100644, flags, 17, 1, obj + 30,
                       64, path('/tmp/a5-shared'), obj, kind, 0)

def snapshot(fds):
    records = []
    task = struct.pack('<7I2QI', 1234, 1234, 1, 1000, 1000, 1000, 1000,
                       0x40000000, 0, 16)
    task += b'\0' * 24
    task += b''.join(struct.pack('<2Q', 0xffffffffffffffff,
                                 0xffffffffffffffff) for _ in range(16))
    task += b'a5-fds\0' + b'\0' * 9
    records.append(tlv(1, task))
    regs = bytearray(336)
    for i in range(31):
        struct.pack_into('<Q', regs, i * 8, 0x1000 + i)
    struct.pack_into('<Q', regs, 248, 0x700000)
    struct.pack_into('<Q', regs, 256, 0x400120)
    struct.pack_into('<Q', regs, 264, 0x60001000)
    records.append(tlv(4, struct.pack('<I', len(regs)) + regs + struct.pack('<Q', 0x1234)))
    records.append(tlv(2, struct.pack('<2I12Q2I', 1234, 1234, *([0] * 12), 1, 0)))
    records.append(tlv(3, struct.pack('<3Q5I6Q512s', 0x400000, 0x401000, 0, 5, 3, 0,
                                      2, 0, 1, 10, 1, 0, 0, 0, path('/tmp/a5-exe'))))
    records.extend(tlv(5, item) for item in fds)
    records.append(tlv(6, path('/tmp') + path('/')))
    records.append(tlv(7, struct.pack('<9I', *( [1000] * 8 + [0] )) + struct.pack('<10I', *([0] * 10))))
    records.append(tlv(0xffff, b''))
    body = b''.join(records)
    total = 64 + len(body) + 24
    header = bytearray(struct.pack('<Q I H H I I I I Q I I Q Q', MAGIC, 1, 64, 0, 183,
                                   PAGE, 1234, 1234, 1, len(records), 0, total, 0))
    digest = hashlib.sha256(header + body).digest()[:8]
    header[56:64] = digest
    return bytes(header) + body + struct.pack('<Q I I Q', MAGIC, 1, len(records), int.from_bytes(digest, 'little'))

open(valid, 'wb').write(snapshot([fd(3, 10), fd(7, 11), fd(100, 11)]))
open(pipe, 'wb').write(snapshot([fd(3, 10, kind=2)]))
PY

"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/valid.bin" -D "$TMP/images"
test -s "$TMP/images/files.img"
test -s "$TMP/images/fdinfo-1.img"
python3 - "$TMP/images" <<'PY'
import pathlib, struct, sys
d = pathlib.Path(sys.argv[1])
def messages(name):
    b = (d / name).read_bytes(); off = 8; out = []
    while off < len(b):
        n = struct.unpack_from('<I', b, off)[0]; off += 4
        out.append(b[off:off+n]); off += n
    assert off == len(b)
    return out
assert len(messages('files.img')) == 5
fdinfo = messages('fdinfo-1.img')
assert len(fdinfo) == 3
def varint(b, off):
    v = 0; shift = 0
    while True:
        x = b[off]; off += 1; v |= (x & 127) << shift
        if x < 128: return v, off
        shift += 7
def fields(b):
    out = {}; off = 0
    while off < len(b):
        tag, off = varint(b, off); n, wire = tag >> 3, tag & 7
        if wire == 0: value, off = varint(b, off)
        elif wire == 2:
            size, off = varint(b, off); value = b[off:off+size]; off += size
        else: raise AssertionError(wire)
        out.setdefault(n, []).append(value)
    return out
assert [fields(x)[4][0] for x in fdinfo] == [3, 7, 100]
assert [fields(x)[1][0] for x in fdinfo][1:] == [3, 3]
PY

set +e
"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/pipe.bin" -D "$TMP/pipe-images"
rc=$?
set -e
test "$rc" -eq 1
test ! -e "$TMP/pipe-images/files.img"
echo 'A5_CONVERTER_FDS: PASS'
