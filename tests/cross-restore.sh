#!/bin/sh
# A3 acceptance gate: module snapshot -> converter -> real CRIU restore.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CRIU=${CRIU:-}
CRIT=${CRIT:-}
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
	make -C "$ROOT/userspace/criu-module-convert" >/dev/null 2>&1 ||
		fail "cannot build $CONVERTER"
}
make -C "$ROOT/tests/progs" minimal >/dev/null 2>&1 ||
	fail "cannot build minimal test program"

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

if ! mountpoint -q "$DEBUG_ROOT" 2>/dev/null; then
	mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null ||
		skip "cannot mount debugfs at $DEBUG_ROOT"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/a3-cross-restore.XXXXXX")
IMAGES=$TMP/images
mkdir -p "$IMAGES"
TARGET_OUT=$TMP/target.out
TARGET_ERR=$TMP/target.err
INPUT=$TMP/stdin
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
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

# All standard descriptors are regular files and the process starts in /.
(cd / && exec "$ROOT/tests/progs/minimal" <"$INPUT" >"$TARGET_OUT" 2>"$TARGET_ERR") &
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
printf '%s %s\n' "$PID" "$TMP/snapshot.bin" >"$DEBUG_DIR/dump" ||
	fail "module dump failed"
rmmod criu_kernel || fail "rmmod failed"
MODULE_LOADED=0

"$CONVERTER" "$TMP/snapshot.bin" -D "$IMAGES" ||
	fail "snapshot conversion failed"

# A3 requires the complete image set, including files which later phases may
# populate with more entries.
for image in \
	inventory.img pstree.img core-$PID.img mm-$PID.img pages-1.img \
	pagemap-1.img files.img fdinfo-1.img reg-files.img ids-$PID.img \
	fs-1.img creds-1.img; do
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

RESTORE_LOG=$TMP/restore.log
if ! "$CRIU" restore -D "$IMAGES" --shell-job --restore-detached \
	-v4 --log-file="$RESTORE_LOG"; then
	tail -80 "$RESTORE_LOG" >&2 2>/dev/null || true
	fail "criu restore rejected module images"
fi

PID=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$TARGET_OUT" | head -n 1)
[ -n "$PID" ] || fail "restored process did not report a pid"
[ "$PID" -gt 0 ] && kill -0 "$PID" 2>/dev/null ||
	fail "restored process is not alive at pid $PID"

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
