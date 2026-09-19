#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
reader="$root_dir/userspace/criu-module-convert/snapshot_reader.c"
model="$root_dir/userspace/criu-module-convert/criu_model.c"

grep -q 'CRIU_SNAPSHOT_READER_UNSUPPORTED' "$reader" 2>/dev/null || true
grep -q 'SNAPSHOT_READER_UNSUPPORTED' "$reader"
grep -q 'CRIU_SNAPSHOT_F_PSTREE' "$model"
grep -q 'process_model_touch' "$model"
echo 'A7_UNSUPPORTED: PASS'
