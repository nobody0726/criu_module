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

skip() { echo "A7_CROSS_RESTORE: SKIP: ENVIRONMENT ($*)"; exit 77; }
fail() { echo "A7_CROSS_RESTORE: FAIL: $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || skip "nested Linux guest required"
[ "$(id -u)" -eq 0 ] || skip "root guest required"
case "$(uname -r)" in 5.10.29*) ;; *) skip "requires Linux 5.10.29 QEMU guest" ;; esac
[ -f "$MODULE" ] && [ -x "$CONVERTER" ] || skip "build module and converter first"
[ -n "$CRIU" ] || skip "criu unavailable"
for prog in tree-simple tree-session tree-pgid; do
	[ -x "$ROOT/tests/progs/$prog" ] || skip "missing fixture binary tests/progs/$prog"
done
mkdir -p "$DEBUG_ROOT" /sys/fs/cgroup
grep -q " $DEBUG_ROOT " /proc/mounts || mount -t debugfs none "$DEBUG_ROOT" || skip "cannot mount debugfs"
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || \
	mount -t cgroup2 none /sys/fs/cgroup || skip "cannot mount cgroup2"

stat_fields()
{
	awk '{print $4, $5, $6}' "/proc/$1/stat"
}

assert_stat()
{
	pid=$1 expected_ppid=$2 expected_pgid=$3 expected_sid=$4
	read -r ppid pgid sid <<EOF_STAT
$(stat_fields "$pid")
EOF_STAT
	[ "$expected_ppid" = "-" ] || [ "$ppid" = "$expected_ppid" ] || \
		fail "pid $pid PPid=$ppid expected=$expected_ppid"
	[ "$expected_pgid" = "-" ] || [ "$pgid" = "$expected_pgid" ] || \
		fail "pid $pid Pgid=$pgid expected=$expected_pgid"
	[ "$expected_sid" = "-" ] || [ "$sid" = "$expected_sid" ] || \
		fail "pid $pid Sid=$sid expected=$expected_sid"
}

wait_ready()
{
	file=$1
	for _ in $(seq 1 200); do
		grep -q '^READY ' "$file" && return 0
		sleep 0.05
	done
	return 1
}

wait_markers()
{
	file=$1 expected=$2
	for _ in $(seq 1 200); do
		count=$(grep -c '^MARKER$' "$file" || true)
		[ "$count" -ge "$expected" ] && return 0
		sleep 0.05
	done
	return 1
}

run_restore()
{
	if command -v timeout >/dev/null 2>&1; then
		timeout "$RESTORE_TIMEOUT" "$CRIU" restore "$@"
	else
		/bin/busybox timeout "$RESTORE_TIMEOUT" "$CRIU" restore "$@"
	fi
}

run_case()
(
	set -eu
	label=$1 fixture=$2
	TMP=$(mktemp -d /tmp/a7-cross-restore.XXXXXX)
	loaded=0
	root=0 child=0 leader=0 member=0
	pids=""
	marker_file="$TMP/markers"
	cleanup() {
		rc=$?
		[ "$loaded" = 0 ] || rmmod criu_kernel 2>/dev/null || true
		for pid in $pids; do kill -9 "$pid" 2>/dev/null || true; done
		for pid in $pids; do wait "$pid" 2>/dev/null || true; done
		if [ "$rc" -ne 0 ] && [ "$rc" -ne 77 ]; then
			echo "A7_$label: FAIL rc=$rc" >&2
			for file in fixture.out fixture.err converter.log restore.log; do
				[ ! -f "$TMP/$file" ] || { echo "--- $file ---" >&2; tail -120 "$TMP/$file" >&2; }
			done
			dmesg | tail -80 >&2 || true
		fi
		rm -rf "$TMP"
	}
	trap cleanup EXIT HUP INT TERM
	: >"$TMP/stdin"
	: >"$marker_file"
	(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
		A7_MARKER_FILE="$marker_file" \
		setsid "$ROOT/tests/progs/$fixture" <"$TMP/stdin" \
		>"$TMP/fixture.out" 2>"$TMP/fixture.err") &
	root=$!
	wait_ready "$TMP/fixture.out" || fail "$label fixture did not become ready"
	case "$label" in
	SIMPLE)
		set -- $(sed -n 's/^READY root=\([0-9][0-9]*\) child=\([0-9][0-9]*\)$/\1 \2/p' "$TMP/fixture.out" | head -n 1)
		parsed_root=$1 child=$2
		[ "$parsed_root" = "$root" ] && [ "$child" -gt 0 ]
		pids="$root $child"
		;;
	SESSION)
		set -- $(sed -n 's/^READY root=\([0-9][0-9]*\) child=\([0-9][0-9]*\)$/\1 \2/p' "$TMP/fixture.out" | head -n 1)
		parsed_root=$1 child=$2
		[ "$parsed_root" = "$root" ] && [ "$child" -gt 0 ]
		pids="$root $child"
		;;
	PGID)
		set -- $(sed -n 's/^READY root=\([0-9][0-9]*\) leader=\([0-9][0-9]*\) member=\([0-9][0-9]*\)$/\1 \2 \3/p' "$TMP/fixture.out" | head -n 1)
		parsed_root=$1 leader=$2 member=$3
		[ "$parsed_root" = "$root" ] && [ "$leader" -gt 0 ] && [ "$member" -gt 0 ]
		pids="$root $leader $member"
		;;
	esac
	insmod "$MODULE"; loaded=1
	[ -e "$DEBUG_DIR/dump-tree" ] && [ -e "$DEBUG_DIR/target" ] || skip "dump-tree unavailable"
	printf '%s\n' "$root" >"$DEBUG_DIR/target"
	printf '%s %s\n' "$root" "$TMP/snapshot.bin" >"$DEBUG_DIR/dump-tree"
	rmmod criu_kernel; loaded=0
	[ -s "$TMP/snapshot.bin" ] && [ ! -e "$TMP/snapshot.bin.tmp" ] || fail "$label snapshot was not atomically published"
	"$CONVERTER" "$TMP/snapshot.bin" -D "$TMP/images" >"$TMP/converter.log" 2>&1
	for pid in $pids; do kill -9 "$pid" 2>/dev/null || true; done
	for pid in $pids; do wait "$pid" 2>/dev/null || true; done
	(cd / && run_restore -D "$TMP/images" --shell-job --restore-detached -v4 --log-file="$TMP/restore.log")
	for pid in $pids; do kill -0 "$pid"; done
	case "$label" in
	SIMPLE)
		assert_stat "$root" - "$root" "$root"
		assert_stat "$child" "$root" "$root" "$root"
		;;
	SESSION)
		assert_stat "$root" - "$root" "$root"
		assert_stat "$child" "$root" "$child" "$child"
		;;
	PGID)
		assert_stat "$root" - "$root" "$root"
		assert_stat "$leader" "$root" "$leader" "$root"
		assert_stat "$member" "$root" "$leader" "$root"
		;;
	esac
	before=$(grep -c '^MARKER$' "$marker_file" || true)
	for pid in $pids; do kill -USR1 "$pid"; done
	set -- $pids
	if ! wait_markers "$marker_file" "$((before + $#))"; then
		for pid in $pids; do
			if kill -0 "$pid" 2>/dev/null; then
				echo "A7_$label: pid $pid still alive after SIGUSR1" >&2
			else
				echo "A7_$label: pid $pid died after SIGUSR1" >&2
			fi
			awk '/^(State|SigBlk|SigIgn|SigCgt):/ { print "A7_'$label': pid '$pid' " $0 }' \
				"/proc/$pid/status" >&2 2>/dev/null || true
			echo "A7_$label: pid $pid fd_count=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)" >&2
		done
		fail "$label behavior marker missing"
	fi
	for pid in $pids; do kill -0 "$pid"; done
	echo "A7_$label: PASS (restore, topology, behavior, liveness)"
)

run_case SIMPLE tree-simple
run_case SESSION tree-session
run_case PGID tree-pgid

if dmesg | grep -E 'BUG:|WARNING:|Oops:|scheduling while atomic|possible circular locking'; then
	echo 'A7_CROSS_RESTORE: FAIL (kernel diagnostics)' >&2
	exit 1
fi
echo 'A7_CROSS_RESTORE: PASS'
