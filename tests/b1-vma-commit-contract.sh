#!/bin/sh
# Contract for the B1 kernel COMMIT transaction.  This is intentionally
# source-level until the guest gate builds the patched Linux 5.10.29 tree.
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
patch4="$root_dir/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"
fixture="$root_dir/tests/fixtures/b1-overlap-plan.c"
tmp="${TMPDIR:-/tmp}/b1-vma-commit-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

cc -std=c11 -Wall -Wextra -Werror -I"$root_dir/include" "$fixture" -o "$tmp/b1-overlap-plan"
"$tmp/b1-overlap-plan" > "$tmp/cases.out"

grep -Fq 'non-overlap' "$tmp/cases.out"
grep -Fq 'overlap-low-to-high' "$tmp/cases.out"
grep -Fq 'overlap-high-to-low' "$tmp/cases.out"

grep -Fq 'criu_restore_commit_vmas' "$patch4"
grep -Fq 'criu_restore_unmap_old_vmas' "$patch4"
grep -Fq 'criu_restore_move_vma' "$patch4"
grep -Fq 'criu_restore_mremap_fixed' "$patch4"
grep -Fq 'criu_restore_map_guard_page' "$patch4"
grep -Fq 'criu_restore_find_temp_range' "$patch4"
grep -Fq 'criu_restore_restore_prot' "$patch4"
grep -Fq 'RESTORE_TX_COMMITTING' "$patch4"
grep -Fq 'tx->state = RESTORE_TX_COMMITTING' "$patch4"
grep -Fq 'tx->state = RESTORE_TX_COMMITTED' "$patch4"
grep -Fq 'tx->state = RESTORE_TX_FAILED' "$patch4"
grep -Fq 'force_sig(SIGKILL)' "$patch4"
grep -Fq 'task_pid_nr(current) != tx->plan.target_pid' "$patch4"
grep -Fq 'mmap_write_lock' "$patch4"
grep -Fq 'mmap_write_unlock' "$patch4"
grep -Fq 'MREMAP_FIXED | MREMAP_MAYMOVE' "$patch4"
grep -Fq 'MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS' "$patch4"
grep -Fq 'do_munmap(mm' "$patch4"
grep -Fq 'vm_mmap(NULL' "$patch4"
grep -Fq 'vm_munmap' "$patch4"
grep -Fq 'do_mprotect_pkey' "$patch4"

if grep -Fq 'copy_from_user(vmas' "$patch4" &&
   awk '/static int criu_restore_commit/,/^}/{ print }' "$patch4" | grep -Fq 'copy_from_user(vmas'; then
	echo "COMMIT must not copy or read the userspace VMA array" >&2
	exit 1
fi

echo "B1_VMA_COMMIT_CONTRACT: PASS"
