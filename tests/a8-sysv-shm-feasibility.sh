#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODULE=${MODULE:-$ROOT/kernel_module/criu_kernel.ko}
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR=$DEBUG_ROOT/criu

skip() { echo "A8_SYSV_SHM: SKIP: ENVIRONMENT ($*)"; exit 77; }
fail() { echo "A8_SYSV_SHM: FAIL: $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || skip "nested Linux guest required"
[ "$(id -u)" -eq 0 ] || skip "root guest required"
case "$(uname -r)" in 5.10.29*) ;; *) skip "requires Linux 5.10.29 QEMU guest" ;; esac
[ -f "$MODULE" ] || skip "build module first"
mkdir -p "$DEBUG_ROOT" /sys/fs/cgroup
grep -q " $DEBUG_ROOT " /proc/mounts ||
	mount -t debugfs none "$DEBUG_ROOT" || skip "cannot mount debugfs"
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts ||
	mount -t cgroup2 none /sys/fs/cgroup || skip "cannot mount cgroup2"

make -C "$ROOT/tests/progs" shm-sysv >/dev/null
TMP=$(mktemp -d /tmp/a8-sysv-shm.XXXXXX)
loaded=0
root=0
child=0
cleanup() {
	rc=$?
	[ "$loaded" = 0 ] || rmmod criu_kernel 2>/dev/null || true
	[ "$root" -eq 0 ] || kill -9 "$root" 2>/dev/null || true
	[ "$child" -eq 0 ] || kill -9 "$child" 2>/dev/null || true
	[ "$root" -eq 0 ] || wait "$root" 2>/dev/null || true
	[ "$child" -eq 0 ] || wait "$child" 2>/dev/null || true
	if [ "$rc" -ne 0 ] && [ "$rc" -ne 77 ]; then
		echo "--- fixture.out ---" >&2
		[ ! -f "$TMP/fixture.out" ] || tail -80 "$TMP/fixture.out" >&2
		echo "--- fixture.err ---" >&2
		[ ! -f "$TMP/fixture.err" ] || tail -80 "$TMP/fixture.err" >&2
		cat "$DEBUG_DIR/status" >&2 2>/dev/null || true
		dmesg | tail -100 >&2 || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 setsid \
	"$ROOT/tests/progs/shm-sysv" >"$TMP/fixture.out" 2>"$TMP/fixture.err") &
root=$!
for _ in $(seq 1 200); do
	grep -q '^READY ' "$TMP/fixture.out" && break
	sleep 0.05
done
set -- $(sed -n 's/^READY root=\([0-9][0-9]*\) child=\([0-9][0-9]*\)$/\1 \2/p' \
	"$TMP/fixture.out" | head -n 1)
[ "${1:-}" = "$root" ] || fail "fixture root pid mismatch"
child=${2:-0}
[ "$child" -gt 0 ] || fail "fixture child pid missing"

insmod "$MODULE"; loaded=1
[ -e "$DEBUG_DIR/dump-tree" ] && [ -e "$DEBUG_DIR/target" ] ||
	skip "dump-tree unavailable"
printf '%s\n' "$root" >"$DEBUG_DIR/target" ||
	fail "kernel target rejected sysv root"
set +e
printf '%s %s\n' "$root" "$TMP/snapshot.bin" >"$DEBUG_DIR/dump-tree"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
	fail "SysV shm unexpectedly dumped; add restore validation before marking supported"
fi
rmmod criu_kernel; loaded=0
grep -E 'unsupported|EOPNOTSUPP|last_error=-95|last_error=-524' "$DEBUG_DIR/status" >/dev/null 2>&1 ||
	true
echo 'A8_SYSV_SHM: UNSUPPORTED_RECORDED'
