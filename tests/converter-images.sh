#!/usr/bin/env bash
set -euo pipefail
root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
python3 - "$tmp/snapshot.bin" <<'PY'
import hashlib, struct, sys

out = sys.argv[1]
records = []

def add(kind, payload):
    records.append(struct.pack('<HHIQ', kind, 0, 0, len(payload)) + payload)

def fixed_path(path, size=512):
    raw = path.encode()
    assert len(raw) < size
    return raw + b'\0' * (size - len(raw))

# The task record contains the fixed ABI prefix, Linux 64-bit signal sets,
# sixteen rlimit pairs, and the optional comm extension.
task = struct.pack('<7I2QI', 1234, 1234, 1, 1000, 1000, 1000, 1000,
                   0x40000000, 0, 16)
task += b'\0' * (8 * 3)
task += b''.join(struct.pack('<2Q', 0xffffffffffffffff,
                             0xffffffffffffffff) for _ in range(16))
task += fixed_path('a3-minimal', 16)
add(1, task)

# AArch64 pt_regs: x0..x30, sp, pc, pstate, orig_x0, syscallno, etc.
regs = bytearray(336)
for i in range(31):
    struct.pack_into('<Q', regs, i * 8, 0x1000 + i)
struct.pack_into('<Q', regs, 248, 0x700000)
struct.pack_into('<Q', regs, 256, 0x400120)
struct.pack_into('<Q', regs, 264, 0x60001000)
add(4, struct.pack('<I', len(regs)) + regs)

creds = struct.pack('<9I', 1000, 1000, 1000, 1000, 1000, 1000,
                    1000, 1000, 0)
creds += struct.pack('<10I', *( [0, 0] * 5 ))
add(7, creds)

# mm_record has 12 uint64 address fields.
mm_values = [0x400000, 0x401000, 0x400000, 0x401000,
             0x7ffffff000, 0x500000, 0x501000, 0x7ffffff000,
             0x7ffffff100, 0x7ffffff200, 0x7ffffff300, 0x7ffffff400]
add(2, struct.pack('<2I12Q2I', 1234, 1234, *mm_values, 3, 0))

def vma(start, end, prot, cls, special, policy, flags, dev, ino, path,
        present=0, saved=0, zero=0, file=0):
    return struct.pack('<3Q5I6Q512s', start, end, 0, prot, cls, special,
                       policy, flags, dev, ino, present, saved, zero, file,
                       fixed_path(path))

# CRIU classes: anon-private=0, file-private=3.  The first file VMA is the
# executable and is used to populate mm.exe_file_id.
add(3, vma(0x400000, 0x401000, 5, 3, 0, 2, 0, 1, 10,
           '/tmp/a3-exe', present=1))
add(3, vma(0x500000, 0x501000, 3, 0, 0, 1, 0, 0, 0,
           '[heap]', present=1, saved=1))
add(3, vma(0x7fffffe000, 0x7ffffff000, 3, 0, 0, 1, 2, 0, 0,
           '[stack]', present=1))

def fd(fdno, mode, path):
    return struct.pack('<2I5Q512s', fdno, mode, 0, 0, dev := 1,
                       ino := fdno + 20, 0, fixed_path(path))

add(5, fd(0, 0o20666, '/dev/null'))
add(5, fd(1, 0o100644, '/tmp/a3-stdout'))
add(5, fd(2, 0o100644, '/tmp/a3-stderr'))
add(6, fixed_path('/tmp') + fixed_path('/'))

page = bytes([0x5a]) * 4096
add(9, struct.pack('<Q4I', 0x500000, 1, 4096, 1, len(page)) + page)
add(0xffff, b'')

body = b''.join(records)
header = bytearray(struct.pack('<Q I H H I I I I Q I I Q Q',
                               0x43524955534e5033, 1, 64, 0, 183, 4096,
                               1234, 1234, 1, len(records), 0,
                               64 + len(body) + 24, 0))
raw = header + body
digest = hashlib.sha256(raw).digest()[:8]
raw[56:64] = digest
raw += struct.pack('<Q I I Q', 0x43524955534e5033, 1, len(records),
                   int.from_bytes(digest, 'little'))
open(out, 'wb').write(raw)
PY
"$bin" "$tmp/snapshot.bin" -D "$tmp/images"
for f in inventory.img pstree.img core-1234.img mm-1234.img pagemap-1234.img files.img fdinfo-1.img fs-1234.img creds-1234.img ids-1234.img reg-files.img; do test -s "$tmp/images/$f" || { echo "missing $f" >&2; exit 1; }; done
test -e "$tmp/images/pages-1.img"
python3 - "$tmp/images" <<'PY'
import pathlib, struct, sys

d = pathlib.Path(sys.argv[1])
expected = {
    'inventory.img': 0x58313116,
    'pstree.img': 0x54564319,
    'core-1234.img': 0x54564319,
    'mm-1234.img': 0x54564319,
    'pagemap-1234.img': 0x54564319,
    'files.img': 0x54564319,
    'fdinfo-1.img': 0x54564319,
    'fs-1234.img': 0x54564319,
    'creds-1234.img': 0x54564319,
    'ids-1234.img': 0x54564319,
    'reg-files.img': 0x54564319,
}
for name, magic in expected.items():
    blob = (d / name).read_bytes()
    assert struct.unpack_from('<I', blob)[0] == magic, name
    if name != 'inventory.img':
        assert struct.unpack_from('<I', blob, 4)[0] != 0, name

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

def messages(path, prefix):
    blob = (d / path).read_bytes()
    off = prefix
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

core = fields(messages('core-1234.img', 8)[0])
assert {1, 3, 4, 5, 8}.issubset(core), core.keys()
assert {1, 2, 3, 4, 5, 6}.issubset(fields(core[3][0]))
assert {1, 2, 3, 4}.issubset(fields(core[8][0])), fields(core[8][0]).keys()

mm = fields(messages('mm-1234.img', 8)[0])
assert 12 in mm and len(mm[14]) == 3, mm.keys()
vmas = [fields(item) for item in mm[14]]
assert vmas[0][4][0] == 1 and vmas[0][6][0] == 2
assert vmas[1][4][0] == 0 and vmas[1][6][0] == 34

files = messages('files.img', 8)
assert len(files) >= 6, len(files)
assert all({1, 2, 3}.issubset(fields(item)) for item in files)
assert all({1, 2, 3, 5, 6}.issubset(fields(fields(item)[3][0])) for item in files)

fdinfo = messages('fdinfo-1.img', 8)
assert len(fdinfo) == 3, len(fdinfo)
assert [fields(item)[4][0] for item in fdinfo] == [0, 1, 2]

pagemap = messages('pagemap-1234.img', 8)
assert len(pagemap) == 2, len(pagemap)
assert fields(pagemap[0])[1][0] == 1
assert fields(pagemap[1])[4][0] == 4
assert len((d / 'pages-1.img').read_bytes()) == 4096
print('image headers and protobuf contracts ok')
PY
echo 'CONVERTER_IMAGES: PASS'
