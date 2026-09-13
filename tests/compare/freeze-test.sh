#!/bin/sh
set -eu

ROOT=/sys/kernel/debug/criu
CG=/sys/fs/cgroup
[ "$(id -u)" = 0 ] || exit 1
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
mount -t cgroup2 none "$CG" 2>/dev/null || true
insmod ./kernel_module/criu_kernel.ko
pid=0
cleanup()
{
	# A cgroup-frozen task cannot run its signal handler, so always release an
	# active freeze before terminating and waiting for the fixture.
	[ -e "$ROOT/thaw" ] && printf '1\n' > "$ROOT/thaw" 2>/dev/null || true
	[ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null || true
	[ "$pid" -gt 0 ] && wait "$pid" 2>/dev/null || true
	rmmod criu_kernel 2>/dev/null || true
}
trap cleanup EXIT

[ -e "$ROOT/freeze" ] && [ -e "$ROOT/thaw" ] || exit 1
./tests/progs/busy-counter > /tmp/a2-busy.out &
pid=$!
i=0
while [ ! -s /tmp/a2-busy.out ] && [ "$i" -lt 50 ]; do
	i=$((i + 1))
	sleep 0.01
done
printf '%s\n' "$pid" > "$ROOT/target"
original_cgroup=$(cat "/proc/$pid/cgroup")
running_before=$(awk '{print $14+$15}' "/proc/$pid/stat")
sleep 0.05
running_after=$(awk '{print $14+$15}' "/proc/$pid/stat")
[ "$running_after" -gt "$running_before" ] || {
	echo "A2_FREEZE: task did not run before freeze" >&2
	exit 1
}
printf '1\n' > "$ROOT/freeze"
status=$(cat "$ROOT/status")
printf '%s\n' "$status" | grep -q 'freeze_state=frozen'
printf '%s\n' "$status" | grep -q 'freeze_settled=1'
printf '%s\n' "$status" | grep -q 'freeze_task_count=1'
frozen_before=$(awk '{print $14+$15}' "/proc/$pid/stat")
sleep 0.05
frozen_after=$(awk '{print $14+$15}' "/proc/$pid/stat")
[ "$frozen_before" = "$frozen_after" ] || {
	echo "A2_FREEZE: task continued while frozen" >&2
	exit 1
}
printf '1\n' > "$ROOT/thaw"
sleep 0.05
later=$(awk '{print $14+$15}' "/proc/$pid/stat")
[ "$later" -gt "$frozen_after" ] || {
	echo "A2_FREEZE: task did not resume after thaw" >&2
	exit 1
}
[ "$(cat "/proc/$pid/cgroup")" = "$original_cgroup" ] || {
	echo "A2_FREEZE: cgroup was not restored after thaw" >&2
	exit 1
}
echo "A2_FREEZE: PASS"
