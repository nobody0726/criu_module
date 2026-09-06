#!/bin/sh
set -eu

ROOT=/sys/kernel/debug/criu
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null || true
insmod ./kernel_module/criu_kernel.ko settle_timeout_ms=0
pid=0
cleanup()
{
	[ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null || true
	[ "$pid" -gt 0 ] && wait "$pid" 2>/dev/null || true
	rmmod criu_kernel 2>/dev/null || true
}
trap cleanup EXIT

./tests/progs/busy-counter >/dev/null &
pid=$!
original=$(cat "/proc/$pid/cgroup")
printf '%s\n' "$pid" > "$ROOT/target"
if printf '1\n' > "$ROOT/freeze" 2>/dev/null; then
	echo "A2_FREEZE: timeout unexpectedly succeeded" >&2
	exit 1
fi
cat "$ROOT/status" | grep -q 'freeze_state=idle'
[ "$(cat "/proc/$pid/cgroup")" = "$original" ] || {
	echo "A2_FREEZE: rollback changed cgroup" >&2
	exit 1
}
echo "A2_FREEZE: ROLLBACK PASS"
