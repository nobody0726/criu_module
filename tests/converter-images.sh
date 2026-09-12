#!/usr/bin/env bash
set -euo pipefail
root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
make -C "$root_dir/userspace/criu-module-convert" >/dev/null
python3 - "$tmp/snapshot.bin" <<'PY'
import hashlib, struct, sys
out=sys.argv[1]; rec=[]
def add(t,p): rec.append(struct.pack('<HHIQ',t,0,0,len(p))+p)
add(1, struct.pack('<7I2Q',1234,1234,1,1000,1000,1000,1000,0,0))
add(2, struct.pack('<2I11Q2I',1234,1234,*([0]*11),0,0))
add(6, bytes(1024)); add(7, bytes(72)); add(0xffff,b'')
body=b''.join(rec); h=bytearray(struct.pack('<Q I H H I I I I Q I I Q Q',0x43524955534e5033,1,64,0,62,4096,1234,1234,1,len(rec),0,64+len(body)+24,0))
raw=h+body; digest=hashlib.sha256(raw).digest()[:8]; raw[56:64]=digest
raw += struct.pack('<Q I I Q',0x43524955534e5033,1,len(rec),int.from_bytes(digest,'little'))
open(out,'wb').write(raw)
PY
"$bin" "$tmp/snapshot.bin" -D "$tmp/images"
for f in inventory.img pstree.img core-1234.img mm-1234.img pagemap-1.img fdinfo-1.img fs-1.img creds-1.img reg-files.img; do test -s "$tmp/images/$f" || { echo "missing $f" >&2; exit 1; }; done
test -e "$tmp/images/pages-1.img"
python3 - "$tmp/images" <<'PY'
import pathlib,struct,sys
d=pathlib.Path(sys.argv[1]); expected={'inventory.img':0x58313116,'pstree.img':0x54564319,'core-1234.img':0x54564319,'mm-1234.img':0x54564319,'pagemap-1.img':0x54564319}
for n,m in expected.items():
 b=(d/n).read_bytes(); assert struct.unpack_from('<I',b)[0]==m,n
 assert struct.unpack_from('<I',b,4)[0] != 0 if n!='inventory.img' else True
print('image headers ok')
PY
echo 'CONVERTER_IMAGES: PASS'
