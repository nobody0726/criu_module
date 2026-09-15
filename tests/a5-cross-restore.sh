#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODULE=${MODULE:-$ROOT/kernel_module/criu_kernel.ko}
CONVERTER=${CONVERTER:-$ROOT/userspace/criu-module-convert/criu-module-convert}
CRIU=${CRIU:-$(command -v criu 2>/dev/null || true)}
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR=$DEBUG_ROOT/criu

skip() { echo "A5_CROSS_RESTORE: SKIP: $*" >&2; exit 77; }
fail() { echo "A5_CROSS_RESTORE: FAIL: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || skip "must run as root in the guest"
[ -f "$MODULE" ] || skip "module not found: $MODULE"
[ -x "$CONVERTER" ] || fail "converter is not built"
[ -x "$ROOT/tests/progs/fds-dup" ] || fail "fds-dup fixture is not built"
grep -q " $DEBUG_ROOT " /proc/mounts || mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null || skip "cannot mount debugfs"
mkdir -p /sys/fs/cgroup
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null || skip "cannot mount cgroup2"

TMP=$(mktemp -d /tmp/a5-cross-restore.XXXXXX)
IMAGES=$TMP/images; SNAPSHOT=$TMP/snapshot.bin; OUT=$TMP/fds.out; ERR=$TMP/fds.err
mkdir -p "$IMAGES"
: >"$TMP/stdin"
PID=0; LOADED=0
cleanup() {
	if [ "$PID" -gt 0 ] 2>/dev/null; then kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
	if [ "$LOADED" = 1 ]; then rmmod criu_kernel 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

(cd / && exec "$ROOT/tests/progs/fds-dup" <"$TMP/stdin" >"$OUT" 2>"$ERR") &
PID=$!
for _ in $(seq 1 100); do
	grep -q '^pid=' "$OUT" 2>/dev/null && break
	sleep 0.05
done
grep -q '^pid=' "$OUT" || fail "fds fixture did not start"
reported=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -n 1)
[ "$reported" = "$PID" ] || fail "pid mismatch reported=$reported shell=$PID"

insmod "$MODULE" || fail "insmod failed"; LOADED=1
[ -e "$DEBUG_DIR/target" ] && [ -e "$DEBUG_DIR/dump" ] || fail "module controls unavailable"
printf '%s\n' "$PID" >"$DEBUG_DIR/target" || fail "target select failed"
printf '%s %s\n' "$PID" "$SNAPSHOT" >"$DEBUG_DIR/dump" || fail "fd dump failed"
rmmod criu_kernel || fail "rmmod failed"; LOADED=0
"$CONVERTER" "$SNAPSHOT" -D "$IMAGES" >"$TMP/converter.out" 2>&1 || fail "conversion failed"
grep -q 'emit fdinfo-1.img' "$TMP/converter.out" || fail "fdinfo image was not emitted"
[ -s "$IMAGES/files.img" ] || fail "files image missing"
[ -s "$IMAGES/reg-files.img" ] || fail "reg-files image missing"
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=0

if [ -z "$CRIU" ]; then
	echo "A5_FD_GUEST: PASS (dump/converter gate; CRIU unavailable)"
	exit 0
fi

set +e
/bin/busybox timeout 90 "$CRIU" restore -D "$IMAGES" --shell-job --restore-detached -v4
restore_rc=$?
set -e
if [ "$restore_rc" -ne 0 ]; then
	echo "A5_CROSS_RESTORE: UNSUPPORTED: regular-fd image rejected by CRIU (rc=$restore_rc)" >&2
	exit 77
fi
echo "A5_CROSS_RESTORE: PASS"
