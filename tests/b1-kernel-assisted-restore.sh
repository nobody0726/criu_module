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
"$ROOT/tests/progs/b1-carrier-probe" || b1_fail "carrier failed before bootstrap"
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
(cd /tmp && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
	setsid "$FIXTURE" >"$OUT" 2>"$ERR") &
PID=$!
for _ in $(seq 1 100); do
	grep -q '^pid=' "$OUT" 2>/dev/null && break
	sleep 0.05
done
grep -q '^pid=' "$OUT" || b1_fail "fixture did not start"
reported=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -n 1)
[ "$reported" = "$PID" ] || b1_fail "pid mismatch reported=$reported shell=$PID"

if ! "$CRIU" dump -t "$PID" -D "$IMAGES" --shell-job --leave-running -v4 \
	--log-file="$B1_TMP/dump.log"; then
	echo "--- CRIU dump log ---" >&2
	cat "$B1_TMP/dump.log" >&2 || true
	echo "--- fixture stderr ---" >&2
	cat "$ERR" >&2 || true
	b1_fail "real CRIU dump failed"
fi
python3 - "$IMAGES/core-$reported.img" <<'PY' >/dev/null
import struct, sys

data = open(sys.argv[1], 'rb').read()
payload_len = struct.unpack_from('<I', data, 8)[0]
payload = data[12:12 + payload_len]

def fields(buf):
    i = 0
    while i < len(buf):
        key = 0
        shift = 0
        while True:
            b = buf[i]; i += 1
            key |= (b & 0x7f) << shift
            if not b & 0x80: break
            shift += 7
        n, wt = key >> 3, key & 7
        if wt == 0:
            val = 0; shift = 0
            while True:
                b = buf[i]; i += 1
                val |= (b & 0x7f) << shift
                if not b & 0x80: break
                shift += 7
            yield n, wt, val
        elif wt == 2:
            ln = 0; shift = 0
            while True:
                b = buf[i]; i += 1
                ln |= (b & 0x7f) << shift
                if not b & 0x80: break
                shift += 7
            yield n, wt, buf[i:i + ln]
            i += ln
        else:
            raise SystemExit(f'wire {wt}')

ti = next(v for n, wt, v in fields(payload) if n == 8)
gp = next(v for n, wt, v in fields(ti) if n == 3)
vals = []
for n, wt, v in fields(gp):
    if n == 1 and wt == 2:
        vals = [x for x, _, _ in fields(v)]
    elif n in (2, 3, 4):
        print('RAW', n, hex(v))
print('RAW_REGS', [hex(x) for x in vals[:6]])
open('/tmp/b1-pc.txt', 'w').write(str(next(v for n, wt, v in fields(gp) if n == 3)))
PY
pc=$(cat /tmp/b1-pc.txt)
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=0
[ ! -d "/proc/$reported" ] || b1_fail "original PID still live after kill"

"$RESTORE" --images "$IMAGES" --dry-run >"$B1_TMP/dry-run.log" 2>&1 ||
	{
		echo "--- dry-run log ---" >&2
		cat "$B1_TMP/dry-run.log" >&2 || true
		b1_fail "real CRIU image parser rejected its own dump"
	}
grep -Fq 'B1_RESTORE: DRY_RUN_OK' "$B1_TMP/dry-run.log" ||
	b1_fail "dry-run lacked the real-image success marker"
cat "$B1_TMP/dry-run.log" >&2

if "$RESTORE" --images "$IMAGES" >"$B1_TMP/restore.log" 2>&1; then
	:
else
	restore_rc=$?
	echo "--- restore failure dmesg ---" >&2
	dmesg | tail -n 80 >&2 || true
	if grep -Fq 'open /dev/criu_restore' "$B1_TMP/restore.log"; then
		b1_skip "patched /dev/criu_restore is not available in this guest"
	fi
	b1_fail "mini-restore live path failed rc=$restore_rc: $(cat "$B1_TMP/restore.log" 2>/dev/null || true)"
fi
if grep -Fq 'open /dev/criu_restore' "$B1_TMP/restore.log"; then
	b1_skip "patched /dev/criu_restore is not available in this guest"
fi

restored_alive=0
for _ in $(seq 1 100); do
	if kill -0 "$reported" 2>/dev/null; then
		restored_alive=1
		break
	fi
	sleep 0.05
done
[ "$restored_alive" = 1 ] || {
	echo "--- restore log ---" >&2
	cat "$B1_TMP/restore.log" >&2 || true
	echo "--- dmesg ---" >&2
	dmesg | grep -Ei 'criu_restore|BUG|Oops|segfault|signal|SIG' >&2 || true
	b1_fail "restored PID $reported is not alive"
}

test -r "/proc/$reported/stat" || b1_fail "restored PID stat is unavailable"
maps_size=$(wc -c <"/proc/$reported/maps")
[ "$maps_size" -gt 0 ] || b1_fail "restored PID has no maps"
grep -q '400000' "/proc/$reported/maps" ||
	b1_fail "restored PID maps do not contain restored image"

b1_dmesg_check "$DMESG_MARK"
echo "B1_KERNEL_ASSISTED_RESTORE: PASS pid=$reported maps_bytes=$maps_size"
