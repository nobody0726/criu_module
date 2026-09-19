#!/usr/bin/env bash
set -eu

root_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$root_dir"

if [ "$(uname -s)" != Linux ]; then
	echo "A8_SMOKE: SKIP: run inside Lima criu-dev" >&2
	exit 77
fi

if [ -z "${TMPDIR:-}" ]; then
	TMPDIR="/run/user/$(id -u)/criu-module-a8-smoke"
	export TMPDIR
fi
mkdir -p "$TMPDIR"

make -C userspace/criu-module-convert clean all >/dev/null
make -C kernel_module clean all >/dev/null

bash tests/converter-images.sh
bash tests/converter-format.sh
bash tests/snapshot-format.sh
bash tests/a5-converter-fds.sh
bash tests/a6-converter-images.sh
bash tests/a7-converter-images.sh
bash tests/a7-dump-transaction.sh
bash tests/a8-abi-contract.sh
bash tests/a8-objmap-scope-contract.sh
bash tests/a8-task-ids-contract.sh
bash tests/a8-shared-fdtable.sh
bash tests/a8-cross-ipc.sh
bash tests/a8-shmem-contract.sh

if [ -z "${CRIU_SOURCE:-}" ] &&
   [ -d /Users/yhome/workspace/source_code/criu_module/criu ]; then
	export CRIU_SOURCE=/Users/yhome/workspace/source_code/criu_module/criu
fi

./scripts/run-qemu.sh --ci --script tests/a8-cross-restore-fd.sh
./scripts/run-qemu.sh --ci --script tests/a8-cross-restore-shmem.sh
./scripts/run-qemu.sh --ci --script tests/a8-sysv-shm-feasibility.sh

echo 'A8_SMOKE: PASS'
