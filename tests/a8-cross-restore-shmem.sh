#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODULE=${MODULE:-$ROOT/kernel_module/criu_kernel.ko}
CONVERTER=${CONVERTER:-$ROOT/userspace/criu-module-convert/criu-module-convert}
CRIU=${CRIU:-$(command -v criu 2>/dev/null || true)}
for candidate in "$ROOT/criu/criu/criu" "$ROOT/criu-bin"; do
	[ -n "$CRIU" ] || [ ! -x "$candidate" ] || CRIU=$candidate
done
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR=$DEBUG_ROOT/criu
RESTORE_TIMEOUT=${RESTORE_TIMEOUT:-90}

skip() { echo "A8_SHMEM_CROSS_RESTORE: SKIP: ENVIRONMENT ($*)"; exit 77; }
fail() { echo "A8_SHMEM_CROSS_RESTORE: FAIL: $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || skip "nested Linux guest required"
[ "$(id -u)" -eq 0 ] || skip "root guest required"
case "$(uname -r)" in 5.10.29*) ;; *) skip "requires Linux 5.10.29 QEMU guest" ;; esac
[ -f "$MODULE" ] && [ -x "$CONVERTER" ] || skip "build module and converter first"
[ -n "$CRIU" ] || skip "criu unavailable"
mkdir -p "$DEBUG_ROOT" /sys/fs/cgroup
grep -q " $DEBUG_ROOT " /proc/mounts ||
	mount -t debugfs none "$DEBUG_ROOT" || skip "cannot mount debugfs"
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts ||
	mount -t cgroup2 none /sys/fs/cgroup || skip "cannot mount cgroup2"

make -C "$ROOT/tests/progs" shm-anon shm-posix >/dev/null
TMP=$(mktemp -d /tmp/a8-shmem-restore.XXXXXX)
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
		echo "A8_SHMEM_CROSS_RESTORE: FAIL rc=$rc" >&2
		for file in fixture.out fixture.err converter.log restore.log; do
			[ ! -f "$TMP/$file" ] || { echo "--- $file ---" >&2; tail -160 "$TMP/$file" >&2; }
		done
		find "$TMP" -maxdepth 2 -type f -printf '%p %s\n' >&2 || true
		dmesg | tail -120 >&2 || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

: >"$TMP/stdin"
(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 setsid \
	"$ROOT/tests/progs/shm-anon" <"$TMP/stdin" \
	>"$TMP/fixture.out" 2>"$TMP/fixture.err") &
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
	fail "kernel target rejected shmem root"
printf '%s %s\n' "$root" "$TMP/snapshot.bin" >"$DEBUG_DIR/dump-tree" ||
	fail "kernel dump-tree rejected shmem fixture"
rmmod criu_kernel; loaded=0
echo "A8_SHMEM_CROSS_RESTORE: dump complete"
[ -s "$TMP/snapshot.bin" ] && [ ! -e "$TMP/snapshot.bin.tmp" ] ||
	fail "snapshot was not atomically published"
"$CONVERTER" "$TMP/snapshot.bin" -D "$TMP/images" >"$TMP/converter.log" 2>&1
echo "A8_SHMEM_CROSS_RESTORE: conversion complete"
set -- "$TMP/images"/pagemap-shmem-*.img
[ -e "$1" ] && [ "$#" -eq 1 ] || fail "expected exactly one shmem pagemap"
size=$(stat -c %s "$1")
[ "$size" -lt 20000 ] || fail "shared memory appears duplicated in image"
kill -9 "$root" "$child" 2>/dev/null || true
wait "$root" 2>/dev/null || true
wait "$child" 2>/dev/null || true
restored_root=$root
restored_child=$child
root=0
child=0

if command -v timeout >/dev/null 2>&1; then
	timeout "$RESTORE_TIMEOUT" "$CRIU" restore -D "$TMP/images" \
		--shell-job --restore-detached -v4 --log-file="$TMP/restore.log"
else
	/bin/busybox timeout "$RESTORE_TIMEOUT" "$CRIU" restore -D "$TMP/images" \
		--shell-job --restore-detached -v4 --log-file="$TMP/restore.log"
fi
echo "A8_SHMEM_CROSS_RESTORE: restore complete"
root=$restored_root
child=$restored_child
kill -0 "$root" || fail "restored root pid is not alive"
kill -0 "$child" || fail "restored child pid is not alive"
printf x >>"$TMP/stdin"
for _ in $(seq 1 300); do
	grep -q '^shmem-check=PASS anon=PASS$' "$TMP/fixture.out" && break
	sleep 0.05
done
grep -q '^shmem-check=PASS anon=PASS$' "$TMP/fixture.out" ||
	fail "shared-memory behavior marker missing"
grep -q '^child-shmem-check=PASS$' "$TMP/fixture.out" ||
	fail "child shared-memory marker missing"
kill -0 "$root"
kill -0 "$child"
if dmesg | grep -E 'BUG:|WARNING:|Oops:|scheduling while atomic|possible circular locking|refcount_t:|KASAN:'; then
	fail "guest dmesg contains kernel diagnostics"
fi
echo 'A8_SHMEM_CROSS_RESTORE: PASS'
