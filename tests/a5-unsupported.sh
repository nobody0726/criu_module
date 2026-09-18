#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
[ "$(id -u)" = 0 ] && [ "$(uname -r)" = 5.10.29 ] || {
	echo 'A5_UNSUPPORTED: SKIP (requires 5.10.29 root guest)'; exit 77;
}
TMP=$(mktemp -d /tmp/a5-unsupported.XXXXXX)
pid=0; loaded=0
cleanup() {
	if [ "$loaded" = 1 ]; then rmmod criu_kernel || true; fi
	if [ "$pid" -gt 0 ]; then kill -9 "-$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p /sys/fs/cgroup
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || mount -t cgroup2 none /sys/fs/cgroup
insmod "$ROOT/kernel_module/criu_kernel.ko"; loaded=1
for kind in fown deleted clone-files thread-files listener pathname dgram seqpacket rights credentials passcred fifo packet-pipe lock external external-pipe shared-pipe; do
	mkdir "$TMP/$kind"
	: >"$TMP/$kind/in"
	(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 setsid \
		"$ROOT/tests/progs/fds-unsupported" "$kind" "$TMP/$kind/object" \
		<"$TMP/$kind/in" >"$TMP/$kind/out" 2>"$TMP/$kind/err") &
	pid=$!
	for _ in $(seq 1 100); do
		grep -q '^pid=' "$TMP/$kind/out" && break
		sleep 0.05
	done
	grep -q "^pid=$pid " "$TMP/$kind/out"
	printf '%s\n' "$pid" >/sys/kernel/debug/criu/target
	"$ROOT/tests/progs/fds-unsupported" --dump "$pid" "$TMP/$kind/snapshot.bin"
	grep -q 'freeze_state=idle' /sys/kernel/debug/criu/status
	[ ! -e "$TMP/$kind/snapshot.bin" ]
	[ ! -e "$TMP/$kind/snapshot.bin.tmp" ]
	printf x >>"$TMP/$kind/in"
	for _ in $(seq 1 100); do
		grep -q '^alive=1$' "$TMP/$kind/out" && break
		sleep 0.05
	done
	grep -q '^alive=1$' "$TMP/$kind/out"
	kill -9 "-$pid"; wait "$pid" 2>/dev/null || true; pid=0
	echo "A5_UNSUPPORTED: $kind PASS (rejected, rollback, target running)"
done
if dmesg | grep -E 'BUG:|WARNING:|Oops:|scheduling while atomic|possible circular locking'; then exit 1; fi
echo 'A5_UNSUPPORTED: PASS'
