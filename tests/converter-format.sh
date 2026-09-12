#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null
cp "$root_dir/tests/fixtures/snapshot-minimal.bin" "$tmp/valid.bin"

"$bin" "$tmp/valid.bin" -D "$tmp/images"
test ! -e "$tmp/images/inventory.img"

python3 - "$tmp" <<'PY'
import hashlib, pathlib, struct, sys
d = pathlib.Path(sys.argv[1])
raw = (d / 'valid.bin').read_bytes()
def write(name, blob): (d / name).write_bytes(blob)
write('truncated.bin', raw[:-3])
x = bytearray(raw); x[56] ^= 1; write('bad-checksum.bin', x)
x = bytearray(raw); x[8] = 2; write('bad-version.bin', x)
x = bytearray(raw); struct.pack_into('<Q', x, 72, 0xffffffffffffffff); write('overflow.bin', x)
# Replace the TASK type with an unknown mandatory type and recompute checksum.
x = bytearray(raw); struct.pack_into('<H', x, 64, 99)
x[56:64] = b'\0' * 8
digest = hashlib.sha256(x[:-24]).digest()[:8]
x[56:64] = digest
struct.pack_into('<Q', x, len(x)-8, int.from_bytes(digest, 'little'))
write('unknown.bin', x)
PY

set +e
"$bin" "$tmp/truncated.bin"; rc=$?; test "$rc" -eq 4
"$bin" "$tmp/bad-checksum.bin"; rc=$?; test "$rc" -eq 4
"$bin" "$tmp/bad-version.bin"; rc=$?; test "$rc" -eq 4
"$bin" "$tmp/overflow.bin"; rc=$?; test "$rc" -eq 4
"$bin" "$tmp/unknown.bin"; rc=$?; test "$rc" -eq 1
set -e

echo 'CONVERTER_FORMAT: PASS'
