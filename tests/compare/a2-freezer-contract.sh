#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
preflight="$repo_dir/tests/compare/freezer-symbols.sh"
wrapper="$repo_dir/patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch"

if grep -q 'CGROUP_ROOT/cgroup\.freeze' "$preflight"; then
	echo "A2_FREEZER_CONTRACT: root cgroup freeze check remains" >&2
	exit 1
fi

config_loop=$(sed -n '/for key in CONFIG_CGROUPS/,/done/p' "$preflight")
printf '%s\n' "$config_loop" | grep -qx '	for key in CONFIG_CGROUPS; do'

grep -q 'cgroup_freeze(temporary, true);' "$wrapper"
grep -q 'Keep the cookie live so the caller can retry thaw' "$wrapper"
grep -q 'keep refs for a retry of cgroup_rmdir' "$wrapper"
grep -q 'cgroup_put(temporary);' "$wrapper"
grep -q 'put_task_struct(leader);' "$wrapper"

echo "A2_FREEZER_CONTRACT: PASS"
