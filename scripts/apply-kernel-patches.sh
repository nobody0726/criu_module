#!/usr/bin/env bash
# Apply the project's source-level changes to the pinned Linux tree.
set -euo pipefail

KDIR="${1:-${KDIR:-$HOME/kernels/linux-5.10.29}}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0001-criu-cgroup-freezer-wrapper.patch"
PROCESS_SET_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0002-criu-cgroup-process-set-freezer.patch"
PROCESS_SET_HEADER_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0003-criu-freezer-process-set-header.patch"
B1_RESTORE_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"
B1_SIGFRAME_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0007-b1-bootstrap-sigframe.patch"
B1_ICACHE_PATCH_FILE="$PROJECT_DIR/patches/linux-5.10.29/0008-b1-icache-flush.patch"

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
if [ ! -f "$B1_RESTORE_PATCH_FILE" ]; then
	echo "kernel patch is missing: $B1_RESTORE_PATCH_FILE" >&2
	exit 1
fi
if [ ! -f "$B1_SIGFRAME_PATCH_FILE" ]; then
	echo "kernel patch is missing: $B1_SIGFRAME_PATCH_FILE" >&2
	exit 1
fi
if [ ! -f "$B1_ICACHE_PATCH_FILE" ]; then
	echo "kernel patch is missing: $B1_ICACHE_PATCH_FILE" >&2
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
else
	if ! patch --dry-run -p1 < "$PROCESS_SET_HEADER_PATCH_FILE" >/dev/null; then
		echo "A7_FREEZER: PROCESS_SET_HEADER_PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	if ! patch --dry-run -p1 < "$PROCESS_SET_PATCH_FILE" >/dev/null; then
		echo "A7_FREEZER: PROCESS_SET_PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	patch -p1 < "$PROCESS_SET_HEADER_PATCH_FILE"
	patch -p1 < "$PROCESS_SET_PATCH_FILE"
	echo "A7_FREEZER: PROCESS_SET_PATCH_APPLIED: $KDIR"
fi
if [ -f kernel/criu_restore.c ] &&
	grep -q 'misc_register(&criu_restore_miscdev)' kernel/criu_restore.c 2>/dev/null &&
	grep -q 'obj-y += criu_restore.o' kernel/Makefile 2>/dev/null; then
	echo "B1_RESTORE: PATCH_ALREADY_APPLIED: $KDIR"
	:
else
	if ! patch --dry-run -p1 < "$B1_RESTORE_PATCH_FILE" >/dev/null; then
		echo "B1_RESTORE: PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	patch -p1 < "$B1_RESTORE_PATCH_FILE"
	echo "B1_RESTORE: PATCH_APPLIED: $KDIR"
fi
if grep -q 'sigframe_staging_sp) &&' kernel/criu_restore.c 2>/dev/null &&
	grep -q 'plan->bootstrap_stack_end' kernel/criu_restore.c 2>/dev/null; then
	echo "B1_SIGFRAME: PATCH_ALREADY_APPLIED: $KDIR"
else
	if ! patch --dry-run -p1 < "$B1_SIGFRAME_PATCH_FILE" >/dev/null; then
		echo "B1_SIGFRAME: PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	patch -p1 < "$B1_SIGFRAME_PATCH_FILE"
	echo "B1_SIGFRAME: PATCH_APPLIED: $KDIR"
fi
if grep -q 'flush_icache_range(vma->target_start' kernel/criu_restore.c 2>/dev/null; then
	echo "B1_ICACHE: PATCH_ALREADY_APPLIED: $KDIR"
else
	if ! patch --dry-run -p1 < "$B1_ICACHE_PATCH_FILE" >/dev/null; then
		echo "B1_ICACHE: PATCH_FAILED: $KDIR" >&2
		exit 1
	fi
	patch -p1 < "$B1_ICACHE_PATCH_FILE"
	echo "B1_ICACHE: PATCH_APPLIED: $KDIR"
fi
