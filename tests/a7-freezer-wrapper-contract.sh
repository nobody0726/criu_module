#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
patch1="$root_dir/patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch"
patch2="$root_dir/patches/linux-5.10.29/0002-criu-cgroup-process-set-freezer.patch"
public_h="$root_dir/include/linux/criu_freezer.h"
module_h="$root_dir/kernel_module/include/criu_freezer.h"
apply="$root_dir/scripts/apply-kernel-patches.sh"

for f in "$patch1" "$patch2" "$public_h" "$module_h" "$apply"; do
	test -s "$f"
done

grep -q 'criu_cgroup_freeze_process_set' "$patch2"
grep -q 'EXPORT_SYMBOL_GPL(criu_cgroup_freeze_process_set)' "$patch2"
grep -q 'cgroup_threadgroup_change_begin' "$patch2"
grep -q 'cgroup_threadgroup_change_end' "$patch2"
grep -q 'cgroup_attach_task(temporary' "$patch2"
grep -q 'cgroup_freeze(temporary, true)' "$patch2"
grep -q 'while (attached)' "$patch2"
grep -q 'criu_freezer_thaw_process_set' "$patch2"
grep -q '0002-criu-cgroup-process-set-freezer.patch' "$apply"
python3 - "$patch2" <<'PY'
from pathlib import Path
import sys

patch = Path(sys.argv[1]).read_text()
begin = patch.count('cgroup_threadgroup_change_begin(')
end = patch.count('cgroup_threadgroup_change_end(')
if begin != 2:
    raise SystemExit(f'process-set wrapper must acquire cgroup_threadgroup_rwsem once per freeze/thaw path, found {begin}')
if end != 1:
    raise SystemExit(f'process-set wrapper must centralize cgroup_threadgroup_rwsem release in one helper, found {end}')
if 'cgroup_threadgroup_change_begin(leaders[i])' in patch:
    raise SystemExit('process-set wrapper must not recursively acquire threadgroup rwsem per leader')
if 'cgroup_threadgroup_change_begin(ctx->leaders[i])' in patch:
    raise SystemExit('process-set thaw must not recursively acquire threadgroup rwsem per leader')
PY

if rg -n '\bcgroup_attach_task\b|\bcgroup_freeze\b' "$root_dir/kernel_module" \
	--glob '*.c' --glob '*.h'; then
	echo 'kernel module must use the exported freezer wrapper only' >&2
	exit 1
fi

echo 'A7_FREEZER_WRAPPER_CONTRACT: PASS'
