#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
bin="$root_dir/userspace/criu-module-convert/criu-module-convert"

if [[ "${1:-}" == "--kernel-contract" ]]; then
	signals="$root_dir/kernel_module/checkpoint/dump_signals.c"
	test -s "$signals"
	rg -q 'spin_lock_irqsave' "$signals"
	rg -q 'spin_unlock_irqrestore' "$signals"
	! rg -q 'kernel_write' "$signals"
	! awk '/spin_lock_irqsave/,/spin_unlock_irqrestore/ { if (/criu_snapshot_writer_record/) exit 1 }' "$signals"
	test -s "$root_dir/tests/progs/sig-handlers.c"
	test -s "$root_dir/tests/progs/sig-pending.c"
	echo 'A6_KERNEL_CONTRACT: PASS'
	exit 0
fi

if [[ "${1:-}" == "--timer-locks" ]]; then
	timers="$root_dir/kernel_module/checkpoint/dump_timers.c"
	test -s "$timers"
	rg -q 'spin_lock_irqsave\(&timer->it_lock' "$timers"
	rg -q 'spin_lock\(&leader->sighand->siglock' "$timers"
	! awk '/spin_lock_irqsave\(&leader->sighand->siglock/,/spin_unlock_irqrestore\(&timer->it_lock/ { if (/it_lock/) exit 1 }' "$timers"
	test -s "$root_dir/tests/progs/timers.c"
	echo 'A6_TIMER_LOCKS: PASS'
	exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$root_dir/tests/fixtures/a6-snapshot-builder.py" "$tmp"
make -C "$root_dir/userspace/criu-module-convert" clean all >/dev/null

"$bin" "$tmp/a6-valid.bin" -D "$tmp/valid-images"

set +e
"$bin" "$tmp/a6-missing.bin" -D "$tmp/missing-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-duplicate.bin" -D "$tmp/duplicate-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-queue-gap.bin" -D "$tmp/gap-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-bad-siginfo.bin" -D "$tmp/bad-siginfo-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-unknown-header-flag.bin" -D "$tmp/unknown-header-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-record-without-flag.bin" -D "$tmp/no-flag-images"; rc=$?
test "$rc" -eq 4
"$bin" "$tmp/a6-unknown-mandatory.bin" -D "$tmp/unknown-images"; rc=$?
test "$rc" -eq 1
set -e

test ! -e "$tmp/missing-images"
test ! -e "$tmp/duplicate-images"
test ! -e "$tmp/gap-images"
test ! -e "$tmp/bad-siginfo-images"
test ! -e "$tmp/unknown-header-images"
test ! -e "$tmp/no-flag-images"
test ! -e "$tmp/unknown-images"
echo 'A6_ABI_CONTRACT: PASS'
