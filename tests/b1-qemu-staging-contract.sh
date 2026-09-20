#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
script="$root_dir/scripts/run-qemu.sh"

grep -Fq 'make -C "$QEMU_PROJECT_DIR/userspace/mini-restore" clean all' "$script" ||
	{
		echo "run-qemu.sh does not rebuild mini-restore in Linux staging tree" >&2
		exit 1
	}
grep -Fq 'userspace/mini-restore/Makefile' "$script" ||
	{
		echo "run-qemu.sh does not gate mini-restore staging on its Makefile" >&2
		exit 1
	}

echo "B1_QEMU_STAGING_CONTRACT: PASS"
