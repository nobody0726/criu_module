#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

make -C "$ROOT/userspace/criu-module-convert" clean all LDFLAGS= >/dev/null
python3 - "$TMP" <<'PY'
import hashlib, struct, sys
from pathlib import Path

d = Path(sys.argv[1])
MAGIC = 0x43524955534e5033
PAGE = 4096

def path(s):
    b = s.encode()
    return b + b'\0' * (512 - len(b))

def tlv(kind, payload):
    return struct.pack('<HHIQ', kind, 0, 0, len(payload)) + payload

def fd(n, obj, flags=0x202, kind=1, fd_flags=0):
    return struct.pack('<2I5Q512sQ2I', n, 0o100644, flags, 17, 1, obj + 30,
                       64, path('/tmp/a5-shared'), obj, kind, fd_flags)

def snapshot(fds, extra=()):
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
    records.extend(extra)
    records.append(tlv(0xffff, b''))
    body = b''.join(records)
    total = 64 + len(body) + 24
    header = bytearray(struct.pack('<Q I H H I I I I Q I I Q Q', MAGIC, 1, 64, 0, 183,
                                   PAGE, 1234, 1234, 1, len(records), 0, total, 0))
    digest = hashlib.sha256(header + body).digest()[:8]
    header[56:64] = digest
    return bytes(header) + body + struct.pack('<Q I I Q', MAGIC, 1, len(records), int.from_bytes(digest, 'little'))

(d / 'valid.bin').write_bytes(snapshot([fd(3, 10), fd(7, 11), fd(100, 11, fd_flags=1)]))
def mapped_fd(n, obj, pos):
    b = bytearray(fd(n, obj))
    struct.pack_into('<Q', b, 16, pos)
    struct.pack_into('<Q', b, 32, 10)
    b[48:560] = path('/tmp/a5-exe')
    return bytes(b)
(d / 'mapped.bin').write_bytes(snapshot([mapped_fd(3, 10, 3),
    mapped_fd(7, 11, 7), mapped_fd(100, 11, 7)]))
pipe_records = [
    tlv(11, struct.pack('<IIQQII', 1, 1, 10, 99, 1, 0)),
    tlv(12, struct.pack('<IIQQII', 1, 0, 99, 4096, 3, 0) + b'abc'),
]
(d / 'pipe.bin').write_bytes(snapshot([fd(3, 10, flags=0, kind=2)], pipe_records))
def usk(obj, peer, kind=1):
    return tlv(13, struct.pack('<IIQQIIIIQ', 1, 0, obj, peer, 1, kind, 1, 2,
                               (212992 << 32) | 212992))
def queue(obj, data=b'hello', scm=0):
    return tlv(14, struct.pack('<IIQIIQ', 1, 0, obj, len(data), scm, 0) + data)
def socket_fd(n, obj, ino):
    b = bytearray(fd(n, obj, kind=3))
    struct.pack_into('<Q', b, 32, ino)
    return bytes(b)
unix_fds = [fd(3, 30, kind=3), fd(7, 31, kind=3), fd(100, 30, kind=3)]
unix_records = [usk(30, 31), usk(31, 30), queue(30), queue(31, b'world')]
(d / 'unix.bin').write_bytes(snapshot(unix_fds, unix_records))
bad = {
    'bad-pipe': ([fd(3, 10, kind=2)], [tlv(11, struct.pack('<IIQQII', 1, 0, 99, 20, 1, 0))]),
    'bad-peer': (unix_fds, [usk(30, 99)] + unix_records[1:]),
    'bad-scm': (unix_fds, unix_records[:2] + [queue(30, scm=1), queue(31)]),
    'bad-type': (unix_fds, [usk(30, 31, 2)] + unix_records[1:]),
    'bad-queue': (unix_fds, unix_records[:2] + [queue(99), queue(31)]),
    'bad-duplicate': (unix_fds, unix_records + [usk(30, 31)]),
    'missing-queue': (unix_fds, unix_records[:2] + [queue(30)]),
    'missing-pipe-data': ([fd(3, 10, flags=0, kind=2)], pipe_records[:1]),
    'self-peer': ([fd(3, 30, kind=3)], [usk(30, 30), queue(30)]),
    'orphan-socket': (unix_fds[:1], unix_records),
    'conflicting-pos': ([mapped_fd(3, 10, 3), mapped_fd(7, 10, 7)], []),
    'conflicting-flags': ([fd(3, 10, flags=0), fd(7, 10, flags=2)], []),
    'bad-queue-length': (unix_fds, unix_records[:2] + [queue(30)[:-1], queue(31)]),
    'bad-endpoint-direction': ([fd(3, 10, flags=1, kind=2)], pipe_records),
    'bad-pipe-flags': ([fd(3, 10, flags=0, kind=2)],
        [tlv(11, struct.pack('<IIQQII', 1, 0, 10, 99, 1, 0)), pipe_records[1]]),
    'socket-zero-ino': ([socket_fd(3, 30, 0), fd(7, 31, kind=3)], unix_records),
    'socket-wide-ino': ([socket_fd(3, 30, 2**32+60), fd(7, 31, kind=3)], unix_records),
    'socket-same-ino': ([socket_fd(3, 30, 61), fd(7, 31, kind=3)], unix_records),
}
for name, (fds, records) in bad.items():
    (d / (name + '.bin')).write_bytes(snapshot(fds, records))
PY

"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/valid.bin" -D "$TMP/images"
test -s "$TMP/images/files.img"
test -s "$TMP/images/fdinfo-1.img"
python3 - "$TMP/images/files.img" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(8)
assert b == struct.pack('<II', 0x54564319, 0x56303138), b.hex()
PY
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
assert [fields(x)[2][0] for x in fdinfo] == [0, 0, 1]
PY

"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/mapped.bin" -D "$TMP/mapped-images"
python3 - "$TMP/mapped-images/files.img" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
off = 8; count = 0
while off < len(b):
    size = struct.unpack_from('<I', b, off)[0]
    off += 4 + size; count += 1
assert count == 5, 'mapped backing file merged with independent fd objects'
PY

"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/pipe.bin" -D "$TMP/pipe-images"
test -s "$TMP/pipe-images/pipes-data.img"
python3 - "$TMP/pipe-images/pipes-data.img" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == struct.pack('<II', 0x54564319, 0x56453709), b.hex()
size = struct.unpack_from('<I', b, 8)[0]
assert b[12+size:] == b'abc', 'pipe payload lost or misframed'
PY

"$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/unix.bin" -D "$TMP/unix-images"
test -s "$TMP/unix-images/sk-queues.img"
python3 - "$TMP" <<'PY'
from pathlib import Path
import struct, sys
root = Path(sys.argv[1])
def fields(b):
    def varint(off):
        value = shift = 0
        while True:
            x = b[off]; off += 1; value |= (x & 127) << shift
            if x < 128: return value, off
            shift += 7
    result = {}; off = 0
    while off < len(b):
        tag, off = varint(off)
        value, off = varint(off)
        if tag & 7 == 2:
            value, off = b[off:off+value], off+value
        else: assert tag & 7 == 0
        result[tag >> 3] = value
    return result
def entries(directory, name, magic, raw=False):
    b = (root / directory / name).read_bytes()
    assert b[:8] == struct.pack('<II', 0x54564319, magic)
    off = 8; out = []
    while off < len(b):
        n = struct.unpack_from('<I', b, off)[0]; off += 4
        msg = fields(b[off:off+n]); off += n
        data = b[off:off+msg[2]] if raw else b''
        if raw: off += msg[2]
        out.append((msg, data))
    assert off == len(b)
    return out
for directory, typ in [('pipe-images', 2), ('unix-images', 5)]:
    files = dict((m[2], m) for m, _ in entries(directory, 'files.img', 0x56303138))
    fdinfo = entries(directory, 'fdinfo-1.img', 0x56213732)
    assert all(m[3] == typ and files[m[1]][1] == typ for m, _ in fdinfo)
    regs = entries(directory, 'reg-files.img', 0x50363636)
    assert all(files[m[1]][1] == 1 for m, _ in regs)
    assert len(regs) == 3
    if typ == 5:
        sockets = dict((m[1], m) for m, _ in entries(directory, 'unixsk.img', 0x54373943))
        assert len(sockets) == 2
        assert [m[1] for m, _ in fdinfo] == [2, 3, 2]
        for ident, sock in sockets.items():
            assert all(field in sock for field in range(1, 12))
            assert fields(files[ident][16]) == sock
            assert sock[8] == next(s[2] for k, s in sockets.items() if k != ident)
        queues = entries(directory, 'sk-queues.img', 0x56264026, raw=True)
        assert [(m[1], data) for m, data in queues] == [(2, b'hello'), (3, b'world')]
        assert all(4 not in m and 128 not in m for m, _ in queues)
PY
for case in bad-pipe bad-peer bad-scm bad-type bad-queue bad-duplicate missing-queue missing-pipe-data self-peer orphan-socket conflicting-pos conflicting-flags bad-queue-length bad-endpoint-direction bad-pipe-flags socket-zero-ino socket-wide-ino socket-same-ino; do
	if "$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/$case.bin" -D "$TMP/$case-images" >"$TMP/$case.log" 2>&1; then
		echo "converter accepted $case" >&2; exit 1
	fi
	[ "$case" = bad-queue-length ] || grep -q 'snapshot validated' "$TMP/$case.log"
	test ! -e "$TMP/$case-images"
done
mkdir "$TMP/existing"
printf 'keep' >"$TMP/existing/sentinel"
if "$ROOT/userspace/criu-module-convert/criu-module-convert" "$TMP/valid.bin" -D "$TMP/existing"; then
	echo 'converter overwrote existing directory' >&2; exit 1
fi
test "$(cat "$TMP/existing/sentinel")" = keep
test "$(find "$TMP/existing" -type f | wc -l)" -eq 1
test -z "$(find "$TMP" -name '*.criu-module-tmp.*' -print)"
echo 'A5_CONVERTER_FDS: PASS'
