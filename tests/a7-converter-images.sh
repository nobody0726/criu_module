#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
model="$root_dir/userspace/criu-module-convert/criu_model.c"
reader="$root_dir/userspace/criu-module-convert/snapshot_reader.c"

grep -q 'struct process_model' "$model"
grep -q 'process_model_touch' "$model"
grep -q 'pstree_count' "$model"
grep -q 'CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE' "$model"
grep -q 'pstree_messages' "$model"
grep -q 'mkdtemp' "$root_dir/userspace/criu-module-convert/criu_model.c"
grep -q 'CRIU_SNAPSHOT_F_PSTREE' "$reader"

make -C "$root_dir/userspace/criu-module-convert" clean \
	main.o snapshot_reader.o criu_model.o image_writer.o >/dev/null
echo 'A7_CONVERTER_IMAGES: PASS'
