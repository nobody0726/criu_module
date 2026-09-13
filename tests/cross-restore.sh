#!/bin/sh
# A3 acceptance gate: module snapshot -> converter -> real CRIU restore.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CRIU=${CRIU:-}
CRIT=${CRIT:-}
RESTORE_TIMEOUT=${RESTORE_TIMEOUT:-60}
KEEP_TMP=${KEEP_TMP:-0}
MODULE=${MODULE:-$ROOT/kernel_module/criu_kernel.ko}
CONVERTER=${CONVERTER:-$ROOT/userspace/criu-module-convert/criu-module-convert}
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR=$DEBUG_ROOT/criu

skip()
{
	echo "A3_CROSS_RESTORE: SKIP: $*" >&2
	exit 77
}

fail()
{
	echo "A3_CROSS_RESTORE: FAIL: $*" >&2
	exit 1
}

[ "$(id -u)" = 0 ] || skip "must run as root in the guest"
[ -f "$MODULE" ] || skip "module not found: $MODULE"
[ -x "$CONVERTER" ] || {
	make -C "$ROOT/userspace/criu-module-convert" clean all >/dev/null 2>&1 ||
		fail "cannot build $CONVERTER"
}
if [ ! -x "$ROOT/tests/progs/minimal" ]; then
	fail "minimal test program is not prebuilt; build it in the Lima orchestration guest before booting QEMU"
fi

if [ -z "$CRIU" ]; then
	CRIU=$(command -v criu 2>/dev/null || true)
	[ -n "$CRIU" ] || [ -x "$ROOT/criu/criu/criu" ] ||
		skip "criu binary is unavailable"
	[ -n "$CRIU" ] || CRIU=$ROOT/criu/criu/criu
fi

crit_decode()
{
	image=$1
	output=$2
	if [ -n "$CRIT" ]; then
		"$CRIT" decode -i "$image" --pretty >"$output"
	elif command -v crit >/dev/null 2>&1; then
		crit decode -i "$image" --pretty >"$output"
	elif [ -f "$ROOT/criu/crit/crit/__main__.py" ]; then
		PYTHONPATH="$ROOT/criu/lib:$ROOT/criu/crit" \
			python3 -m crit decode -i "$image" --pretty >"$output"
	else
		return 127
	fi
}

mkdir -p "$DEBUG_ROOT"
if ! grep -q " $DEBUG_ROOT " /proc/mounts; then
	mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null ||
		skip "cannot mount debugfs at $DEBUG_ROOT"
fi
CGROUP_ROOT=/sys/fs/cgroup
mkdir -p "$CGROUP_ROOT"
if ! grep -q " $CGROUP_ROOT cgroup2 " /proc/mounts; then
	mount -t cgroup2 none "$CGROUP_ROOT" 2>/dev/null ||
		skip "cannot mount cgroup2 at $CGROUP_ROOT"
fi

TMP=$(mktemp -d "/tmp/a3-cross-restore.XXXXXX")
IMAGES="$TMP/images"
SNAPSHOT="$TMP/snapshot.bin"
RESTORE_LOG="$TMP/restore.log"
mkdir -p "$IMAGES"
TARGET_OUT="$TMP/target.out"
TARGET_ERR="$TMP/target.err"
INPUT="$TMP/stdin"
: >"$INPUT"
PID=0
MODULE_LOADED=0
cleanup()
{
	if [ "$PID" -gt 0 ] 2>/dev/null; then
		kill -9 "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	if [ "$MODULE_LOADED" = 1 ]; then
		rmmod criu_kernel 2>/dev/null || true
	fi
	if [ "$KEEP_TMP" = 1 ]; then
		echo "A3_CROSS_RESTORE: keeping diagnostics in $TMP" >&2
	else
		rm -rf "$TMP"
	fi
}
trap cleanup EXIT HUP INT TERM

# Give the single dumped task its own session.  This keeps the session leader
# inside the dump set for both the module gate and native-CRIU comparison.
# Linux 5.10.29 does not expose PTRACE_GET_RSEQ_CONFIGURATION, so disable
# glibc's automatic rseq registration for the A3 single-threaded target.
(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
	setsid "$ROOT/tests/progs/minimal" <"$INPUT" >"$TARGET_OUT" 2>"$TARGET_ERR") &
PID=$!
for _ in $(seq 1 100); do
	if grep -q '^pid=' "$TARGET_OUT" 2>/dev/null; then
		break
	fi
	sleep 0.05
done
grep -q '^pid=' "$TARGET_OUT" || fail "minimal did not start (see $TARGET_ERR)"
reported_pid=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$TARGET_OUT" | head -n 1)
[ "$reported_pid" = "$PID" ] ||
	fail "minimal reported pid $reported_pid, shell saw $PID"
last_tick=$(awk -F= '/^tick=/{value=$2} END{print value+0}' "$TARGET_OUT")

insmod "$MODULE" || fail "insmod failed"
MODULE_LOADED=1
[ -e "$DEBUG_DIR/target" ] && [ -e "$DEBUG_DIR/dump" ] ||
	fail "module dump controls are unavailable"
printf '%s\n' "$PID" >"$DEBUG_DIR/target" || fail "selecting target failed"
cat "$DEBUG_DIR/target" >&2 2>/dev/null || true
if ! printf '%s %s\n' "$PID" "$SNAPSHOT" >"$DEBUG_DIR/dump"; then
	cat "$DEBUG_DIR/status" >&2 2>/dev/null || true
	dmesg | tail -80 >&2 || true
	fail "module dump failed"
fi
rmmod criu_kernel || fail "rmmod failed"
MODULE_LOADED=0

"$CONVERTER" "$SNAPSHOT" -D "$IMAGES" ||
	fail "snapshot conversion failed"

# A3 requires the complete image set, including files which later phases may
# populate with more entries.
for image in \
	inventory.img pstree.img core-$PID.img mm-$PID.img pages-1.img \
	pagemap-$PID.img files.img fdinfo-1.img reg-files.img ids-$PID.img \
	fs-$PID.img creds-$PID.img; do
	[ -e "$IMAGES/$image" ] || fail "converter omitted $image"
done

# pages-1.img is raw; every other image must be accepted by crit.
for image in "$IMAGES"/*.img; do
	[ -e "$image" ] || fail "converter emitted no images"
	case "$(basename "$image")" in
	pages-*.img) continue ;;
	esac
	json=$TMP/$(basename "$image").json
	if ! crit_decode "$image" "$json"; then
		fail "crit cannot decode $(basename "$image")"
	fi
done

# Stop the original after all data has been collected.  Appending to stdin
# after restore is the explicit event that asks minimal.c to report its tick.
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=0

# The fallback guest runs under TCG.  A bounded restore makes a failed CRIU
# probe diagnosable instead of leaving the QEMU harness running indefinitely.
set +e
(cd / && /bin/busybox timeout "$RESTORE_TIMEOUT" "$CRIU" \
	restore -D "$IMAGES" --shell-job --restore-detached \
	-v4 --log-file="$RESTORE_LOG")
restore_rc=$?
set -e
if [ "$restore_rc" -ne 0 ]; then
	tail -80 "$RESTORE_LOG" >&2 2>/dev/null || true
	if [ "$restore_rc" -eq 143 ] || [ "$restore_rc" -eq 124 ]; then
		fail "criu restore timed out after ${RESTORE_TIMEOUT}s"
	fi
	fail "criu restore rejected module images (rc=$restore_rc)"
fi

PID=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$TARGET_OUT" | head -n 1)
[ -n "$PID" ] || fail "restored process did not report a pid"
restored_alive=0
for _ in $(seq 1 20); do
	if [ "$PID" -gt 0 ] && kill -0 "$PID" 2>/dev/null; then
		restored_alive=1
		break
	fi
	sleep 0.05
done
if [ "$restored_alive" != 1 ]; then
	echo "restored process is not alive at pid $PID" >&2
	echo "--- restore.log ---" >&2
	tail -120 "$RESTORE_LOG" >&2 2>/dev/null || true
	echo "--- /proc status ---" >&2
	cat "/proc/$PID/status" >&2 2>/dev/null || true
	tail -40 "$TARGET_OUT" >&2 2>/dev/null || true
	tail -40 "$TARGET_ERR" >&2 2>/dev/null || true
	ps 2>/dev/null || true
	dmesg | tail -60 >&2 2>/dev/null || true
	fail "restored process is not alive at pid $PID"
fi

# The file size recorded by the dump stays stable until this point.  The
# append below happens only after restore, so it cannot invalidate reg-file
# identity checks performed by CRIU.
printf 'x' >>"$INPUT"
resumed=0
for _ in $(seq 1 100); do
	new_tick=$(awk -F= '/^tick=/{value=$2} END{print value+0}' "$TARGET_OUT")
	if [ "$new_tick" -gt "$last_tick" ]; then
		resumed=1
		break
	fi
	sleep 0.05
done
[ "$resumed" = 1 ] || fail "restored process did not advance tick ($last_tick)"
if grep -qE 'HEAP CORRUPT|STACK CORRUPT' "$TARGET_OUT"; then
	grep -E 'HEAP CORRUPT|STACK CORRUPT' "$TARGET_OUT" >&2
	fail "restored process detected memory corruption"
fi

kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
echo "A3_CROSS_RESTORE: PASS"
