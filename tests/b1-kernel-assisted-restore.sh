#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/b1-guest-helpers.sh"

[ "$(id -u)" = 0 ] || b1_skip "must run as root in Linux 5.10.29 guest"
case "$(uname -r)" in
5.10.*) ;;
*) b1_skip "expected Linux 5.10.x guest, got $(uname -r)" ;;
esac

B1_TMP=$(mktemp -d /tmp/b1-kernel-assisted.XXXXXX)
export B1_TMP
b1_require_guest_tmp
cleanup() {
	if [ -n "${PID:-}" ] && [ "$PID" -gt 0 ] 2>/dev/null; then
		kill -9 "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	rm -rf "$B1_TMP"
}
trap cleanup EXIT HUP INT TERM

FIXTURE="$ROOT/tests/progs/b1-minimal"
RESTORE="$ROOT/userspace/mini-restore/mini-restore"
CRIU=${CRIU:-$(command -v criu 2>/dev/null || true)}
[ -x "$FIXTURE" ] || b1_fail "fixture not built: $FIXTURE"
[ -x "$RESTORE" ] || b1_fail "mini-restore not built: $RESTORE"
[ -n "$CRIU" ] || [ -x "$ROOT/criu/criu/criu" ] || b1_skip "criu unavailable in guest"
[ -n "$CRIU" ] || CRIU="$ROOT/criu/criu/criu"

OUT="$B1_TMP/fixture.out"
ERR="$B1_TMP/fixture.err"
IMAGES="$B1_TMP/images"
mkdir -p "$IMAGES"

DMESG_MARK=$(b1_dmesg_mark)
(cd /tmp && exec "$FIXTURE" >"$OUT" 2>"$ERR") &
PID=$!
for _ in $(seq 1 100); do
	grep -q '^pid=' "$OUT" 2>/dev/null && break
	sleep 0.05
done
grep -q '^pid=' "$OUT" || b1_fail "fixture did not start"
reported=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -n 1)
[ "$reported" = "$PID" ] || b1_fail "pid mismatch reported=$reported shell=$PID"

"$CRIU" dump -t "$PID" -D "$IMAGES" --shell-job --leave-running -v4 \
	--log-file="$B1_TMP/dump.log" || b1_fail "real CRIU dump failed"
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=0
[ ! -d "/proc/$reported" ] || b1_fail "original PID still live after kill"

"$RESTORE" --images "$IMAGES" --dry-run >"$B1_TMP/dry-run.log" 2>&1 ||
	b1_fail "real CRIU image parser rejected its own dump"
grep -Fq 'B1_RESTORE: DRY_RUN_OK' "$B1_TMP/dry-run.log" ||
	b1_fail "dry-run lacked the real-image success marker"

if "$RESTORE" --images "$IMAGES" >"$B1_TMP/restore.log" 2>&1; then
	b1_fail "mini-restore returned success without a verified restored PID"
fi
if grep -Fq 'open /dev/criu_restore' "$B1_TMP/restore.log"; then
	b1_skip "patched /dev/criu_restore is not available in this guest"
fi
grep -E 'FORMAT|IO|UNSUPPORTED' "$B1_TMP/restore.log" >/dev/null ||
	b1_fail "live restore failure lacked diagnostic"

b1_dmesg_check "$DMESG_MARK"
b1_fail "live restore is not complete; inspect restore.log"
