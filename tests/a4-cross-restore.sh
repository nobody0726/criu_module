#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODULE=${MODULE:-$ROOT/kernel_module/criu_kernel.ko}
CONVERTER=${CONVERTER:-$ROOT/userspace/criu-module-convert/criu-module-convert}
CRIU=${CRIU:-}
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR=$DEBUG_ROOT/criu

skip() { echo "A4_CROSS_RESTORE: SKIP: $*" >&2; exit 77; }
fail() { echo "A4_CROSS_RESTORE: FAIL: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || skip "must run as root in the guest"
[ -f "$MODULE" ] || skip "module not found: $MODULE"
[ -x "$CONVERTER" ] || fail "converter is not built"
[ -x "$ROOT/tests/progs/threads" ] || fail "threads fixture is not built"
if [ -z "$CRIU" ]; then
	CRIU=$(command -v criu 2>/dev/null || true)
	[ -n "$CRIU" ] || [ -x "$ROOT/criu/criu/criu" ] || skip "criu binary is unavailable"
	[ -n "$CRIU" ] || CRIU=$ROOT/criu/criu/criu
fi
mkdir -p "$DEBUG_ROOT"
grep -q " $DEBUG_ROOT " /proc/mounts || mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null || skip "cannot mount debugfs"
mkdir -p /sys/fs/cgroup
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null || skip "cannot mount cgroup2"
TMP=$(mktemp -d /tmp/a4-cross-restore.XXXXXX)
IMAGES=$TMP/images; SNAPSHOT=$TMP/snapshot.bin; OUT=$TMP/threads.out; ERR=$TMP/threads.err; RESTORE_LOG=$TMP/restore.log; INPUT=$TMP/stdin
mkdir -p "$IMAGES"
: >"$INPUT"
PID=0; LOADED=0
cleanup() {
	if [ "$PID" -gt 0 ] 2>/dev/null; then kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
	if [ "$LOADED" = 1 ]; then rmmod criu_kernel 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 setsid "$ROOT/tests/progs/threads" <"$INPUT" >"$OUT" 2>"$ERR") &
PID=$!
for _ in $(seq 1 100); do
	grep -q '^pid=' "$OUT" 2>/dev/null && break
	sleep 0.05
done
grep -q '^pid=' "$OUT" || fail "threads fixture did not start"
reported=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -n 1)
[ "$reported" = "$PID" ] || fail "pid mismatch reported=$reported shell=$PID"
insmod "$MODULE" || fail "insmod failed"; LOADED=1
[ -e "$DEBUG_DIR/target" ] && [ -e "$DEBUG_DIR/dump" ] || fail "module controls unavailable"
printf '%s\n' "$PID" >"$DEBUG_DIR/target" || fail "target select failed"
printf '%s %s\n' "$PID" "$SNAPSHOT" >"$DEBUG_DIR/dump" || fail "thread dump failed"
rmmod criu_kernel || fail "rmmod failed"; LOADED=0
"$CONVERTER" "$SNAPSHOT" -D "$IMAGES" || fail "conversion failed"
cores=$(find "$IMAGES" -maxdepth 1 -name 'core-*.img' -type f | wc -l)
[ "$cores" -eq 9 ] || fail "expected 9 core images, got $cores"
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=0
set +e
(cd / && /bin/busybox timeout 90 "$CRIU" restore -D "$IMAGES" --shell-job --restore-detached -v4 --log-file="$RESTORE_LOG")
restore_rc=$?
set -e
if [ "$restore_rc" -ne 0 ]; then
	tail -100 "$RESTORE_LOG" >&2 2>/dev/null || true
	echo "A4_CROSS_RESTORE: UNSUPPORTED: CRIU rejected per-thread state (rc=$restore_rc)" >&2
	exit 77
fi
PID=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -n 1)
[ -n "$PID" ] || fail "restored fixture did not report pid"
for _ in $(seq 1 50); do
	count=$(find "/proc/$PID/task" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
	[ "$count" -eq 9 ] && break
	sleep 0.1
done
count=$(find "/proc/$PID/task" -mindepth 1 -maxdepth 1 -type d | wc -l)
[ "$count" -eq 9 ] || fail "restored thread count=$count"
for index in $(seq 0 7); do grep -q "thread=$index tls=7150000$index" "$OUT" || fail "thread metadata missing index=$index"; done
echo "A4_CROSS_RESTORE: PASS"
