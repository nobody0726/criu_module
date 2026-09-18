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
skip() { echo "A6_CROSS_RESTORE: SKIP: $*" >&2; exit 77; }
[ "$(id -u)" = 0 ] || skip "root required"
case "$(uname -r)" in 5.10.29*) ;; *) skip "requires Linux 5.10.29 QEMU guest" ;; esac
[ -f "$MODULE" ] && [ -x "$CONVERTER" ] || skip "build module and converter first"
[ -n "$CRIU" ] || skip "CRIU unavailable; no restore verification performed"
mkdir -p "$DEBUG_ROOT" /sys/fs/cgroup
grep -q " $DEBUG_ROOT " /proc/mounts || mount -t debugfs none "$DEBUG_ROOT"
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts ||
	mount -t cgroup2 none /sys/fs/cgroup

run_case() (
	set -eu
	label=$1
	fixture=$2
	marker=$3
	TMP=$(mktemp -d /tmp/a6-cross-restore.XXXXXX)
	pid=0
	loaded=0
	cleanup() {
		rc=$?
		[ "$loaded" = 1 ] && rmmod criu_kernel 2>/dev/null || true
		[ "$pid" -gt 0 ] && kill -9 "$pid" 2>/dev/null || true
		[ "$pid" -gt 0 ] && wait "$pid" 2>/dev/null || true
		if [ "$rc" -ne 0 ]; then
			echo "A6_$label: FAIL rc=$rc" >&2
			for file in fixture.out fixture.err converter.log restore.log; do
				[ ! -f "$TMP/$file" ] || tail -100 "$TMP/$file" >&2
			done
			dmesg | tail -60 >&2
		fi
		rm -rf "$TMP"
	}
	trap cleanup EXIT HUP INT TERM
	: >"$TMP/stdin"
	(cd / && exec setsid "$ROOT/tests/progs/$fixture" <"$TMP/stdin" \
		>"$TMP/fixture.out" 2>"$TMP/fixture.err") &
	pid=$!
	for _ in $(seq 1 200); do
		grep -q '^pid=' "$TMP/fixture.out" && break
		sleep 0.05
	done
	reported=$(sed -n 's/^pid=\([0-9][0-9]*\).*/\1/p' "$TMP/fixture.out" | head -n 1)
	[ "$reported" = "$pid" ]
	insmod "$MODULE"; loaded=1
	printf '%s\n' "$pid" >"$DEBUG_DIR/target"
	printf '%s %s\n' "$pid" "$TMP/snapshot.bin" >"$DEBUG_DIR/dump"
	rmmod criu_kernel; loaded=0
	"$CONVERTER" "$TMP/snapshot.bin" -D "$TMP/images" \
		>"$TMP/converter.log" 2>&1
	kill -9 "$pid"; wait "$pid" 2>/dev/null || true; pid=0
	(cd / && /bin/busybox timeout "${RESTORE_TIMEOUT:-90}" "$CRIU" restore \
		-D "$TMP/images" --shell-job --restore-detached -v4 \
		--log-file="$TMP/restore.log")
	pid=$reported
	kill -0 "$pid"
	[ "$label" != HANDLERS ] || kill -USR1 "$pid"
	for _ in $(seq 1 200); do
		grep -q "^$marker=PASS" "$TMP/fixture.out" && break
		sleep 0.05
	done
	grep -q "^$marker=PASS" "$TMP/fixture.out"
	kill -0 "$pid"
	echo "A6_$label: PASS (restore, behavior, liveness)"
)

run_case HANDLERS sig-handlers handler
run_case PENDING sig-pending pending
run_case TIMERS timers timer

if dmesg | grep -E 'BUG:|WARNING:|Oops:|scheduling while atomic|possible circular locking'; then
	echo 'A6_CROSS_RESTORE: FAIL (kernel diagnostics)' >&2
	exit 1
fi
echo 'A6_CROSS_RESTORE: PASS'
