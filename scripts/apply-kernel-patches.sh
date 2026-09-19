#!/usr/bin/env bash
# Apply the project's source-level changes to the pinned Linux tree.
set -euo pipefail

KDIR="${1:-${KDIR:-$HOME/kernels/linux-5.10.29}}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch"
PROCESS_SET_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0002-criu-cgroup-process-set-freezer.patch"
PROCESS_SET_HEADER_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0003-criu-freezer-process-set-header.patch"

if [ ! -d "$KDIR" ]; then
	echo "kernel tree does not exist: $KDIR" >&2
	exit 1
fi
if [ ! -f "$PATCH_FILE" ]; then
	echo "kernel patch is missing: $PATCH_FILE" >&2
	exit 1
fi
if [ ! -f "$PROCESS_SET_PATCH_FILE" ]; then
	echo "kernel patch is missing: $PROCESS_SET_PATCH_FILE" >&2
	exit 1
fi
if [ ! -f "$PROCESS_SET_HEADER_PATCH_FILE" ]; then
	echo "kernel patch is missing: $PROCESS_SET_HEADER_PATCH_FILE" >&2
	exit 1
fi

cd "$KDIR"
if grep -q 'EXPORT_SYMBOL_GPL(criu_cgroup_freeze_threadgroup)' kernel/cgroup/cgroup.c 2>/dev/null; then
	echo "A2_FREEZER: PATCH_ALREADY_APPLIED: $KDIR"
else
	if ! patch --dry-run -p1 < "$PATCH_FILE" >/dev/null; then
		echo "A2_FREEZER: PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	patch -p1 < "$PATCH_FILE"
	echo "A2_FREEZER: PATCH_APPLIED: $KDIR"
fi
if grep -q 'EXPORT_SYMBOL_GPL(criu_cgroup_freeze_process_set)' kernel/cgroup/cgroup.c 2>/dev/null &&
	grep -q '^int criu_cgroup_freeze_process_set' include/linux/criu_freezer.h 2>/dev/null; then
	echo "A7_FREEZER: PROCESS_SET_ALREADY_APPLIED: $KDIR"
	exit 0
fi
if ! patch --dry-run -p1 < "$PROCESS_SET_HEADER_PATCH_FILE" >/dev/null; then
	echo "A7_FREEZER: PROCESS_SET_HEADER_PATCH_FAILED: $KDIR" >&2
	exit 1
fi
patch -p1 < "$PROCESS_SET_HEADER_PATCH_FILE"
if ! patch --dry-run -p1 < "$PROCESS_SET_PATCH_FILE" >/dev/null; then
	echo "A7_FREEZER: PROCESS_SET_PATCH_FAILED: $KDIR" >&2
	exit 1
fi
patch -p1 < "$PROCESS_SET_PATCH_FILE"
echo "A7_FREEZER: PROCESS_SET_PATCH_APPLIED: $KDIR"
