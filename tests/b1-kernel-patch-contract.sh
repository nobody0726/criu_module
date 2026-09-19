#!/usr/bin/env bash
# Contract for the B1 kernel restore helper patch.  The implementation is a
# scaffold in the pinned Linux tree: it must establish the misc device, state
# machine, authorization boundary, and idempotent patch application path without
# taking on full VMA commit semantics yet.
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
patch4="$root_dir/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"
apply="$root_dir/scripts/apply-kernel-patches.sh"

test -s "$patch4"
test -s "$apply"

grep -q 'kernel/criu_restore.c' "$patch4"
grep -q 'obj-y += criu_restore.o' "$patch4"
grep -q 'MISC_DYNAMIC_MINOR' "$patch4"
grep -q '\.name[[:space:]]*=[[:space:]]*"criu_restore"' "$patch4"
grep -q 'misc_register(&criu_restore_miscdev)' "$patch4"
grep -q 'CRIU_RESTORE_VALIDATE_V1' "$patch4"
grep -q 'CRIU_RESTORE_COMMIT_V1' "$patch4"
grep -q 'RESTORE_TX_OPEN' "$patch4"
grep -q 'RESTORE_TX_VALIDATED' "$patch4"
grep -q 'RESTORE_TX_COMMITTING' "$patch4"
grep -q 'RESTORE_TX_COMMITTED' "$patch4"
grep -q 'RESTORE_TX_FAILED' "$patch4"
grep -q 'capable(CAP_SYS_ADMIN)' "$patch4"
grep -q 'get_current_cred()' "$patch4"
grep -q 'target_pid' "$patch4"
grep -q 'task_pid_nr(current) != tx->plan.target_pid' "$patch4"

if grep -q 'criu_cgroup_freeze_threadgroup\|criu_cgroup_freeze_process_set\|criu_cgroup_thaw_threadgroup' "$patch4"; then
	echo 'B1 restore helper patch must not modify existing A2/A7 freezer semantics' >&2
	exit 1
fi

grep -q '0004-criu-restore-mm-helper.patch' "$apply"
grep -q 'B1_RESTORE: PATCH_APPLIED' "$apply"
grep -q 'B1_RESTORE: PATCH_ALREADY_APPLIED' "$apply"

src_kernel="${KDIR:-$HOME/kernels/linux-5.10.29}"
if [ ! -d "$src_kernel" ]; then
	echo "B1_RESTORE_KERNEL_PATCH_CONTRACT: SKIP_APPLY_TEST missing kernel tree: $src_kernel"
	echo "B1_RESTORE_KERNEL_PATCH_CONTRACT: PASS"
	exit 0
fi

tmp="${TMPDIR:-/tmp}/b1-kernel-patch-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"
kcopy="$tmp/linux-5.10.29"
cp -R "$src_kernel" "$kcopy"

first_log="$tmp/apply-first.log"
second_log="$tmp/apply-second.log"
bash "$apply" "$kcopy" > "$first_log"
bash "$apply" "$kcopy" > "$second_log"

grep -q 'B1_RESTORE: PATCH_APPLIED' "$first_log"
grep -q 'B1_RESTORE: PATCH_ALREADY_APPLIED' "$second_log"
test -s "$kcopy/kernel/criu_restore.c"
grep -q 'obj-y += criu_restore.o' "$kcopy/kernel/Makefile"

echo "B1_RESTORE_KERNEL_PATCH_CONTRACT: PASS"
