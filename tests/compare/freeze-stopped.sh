#!/bin/sh
set -eu

ROOT=/sys/kernel/debug/criu
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null || true
insmod ./kernel_module/criu_kernel.ko
pid=0
cleanup()
{
	[ "$pid" -gt 0 ] && kill -CONT "$pid" 2>/dev/null || true
	[ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null || true
	[ "$pid" -gt 0 ] && wait "$pid" 2>/dev/null || true
	rmmod criu_kernel 2>/dev/null || true
}
trap cleanup EXIT

./tests/progs/busy-counter >/dev/null &
pid=$!
kill -STOP "$pid"
printf '%s\n' "$pid" > "$ROOT/target"
printf '1\n' > "$ROOT/freeze"
cat "$ROOT/status" | grep -q 'freeze_was_stopped=1'
printf '1\n' > "$ROOT/thaw"
state=$(awk '{print $3}' "/proc/$pid/stat")
[ "$state" = T ] || {
	echo "A2_FREEZE: stopped state was not preserved" >&2
	exit 1
}
echo "A2_FREEZE: STOPPED PASS"
