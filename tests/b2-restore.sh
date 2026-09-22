#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/b1-guest-helpers.sh"

b2_fail() {
	echo "B2_PROCESS_TREE_RESTORE: FAIL: $*" >&2
	exit 1
}

b2_skip() {
	echo "B2_PROCESS_TREE_RESTORE: SKIP: $*" >&2
	exit 77
}

dump_failure_context() {
	echo "B2_DEBUG: criu dump diagnostics" >&2
	if [ -f "$B2_TMP/dump.log" ]; then
		echo "--- dump.log ---" >&2
		cat "$B2_TMP/dump.log" >&2 || true
	fi
	if [ -f "$B2_TMP/fixture.out" ]; then
		echo "--- fixture.out ---" >&2
		cat "$B2_TMP/fixture.out" >&2 || true
	fi
	if [ -f "$B2_TMP/fixture.err" ]; then
		echo "--- fixture.err ---" >&2
		cat "$B2_TMP/fixture.err" >&2 || true
	fi
	echo "--- image directory ---" >&2
	find "$IMAGES" -maxdepth 1 -type f -printf '%f %s bytes\n' 2>/dev/null >&2 || true
	echo "--- process state ---" >&2
	for pid in "$ROOT_PID" "$CHILD_PID"; do
		[ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null || continue
		if [ -r "/proc/$pid/status" ]; then
			echo "### /proc/$pid/status" >&2
			cat "/proc/$pid/status" >&2 || true
			echo "### /proc/$pid/stat" >&2
			cat "/proc/$pid/stat" >&2 || true
			echo "### /proc/$pid/wchan" >&2
			cat "/proc/$pid/wchan" >&2 || true
		else
			echo "pid=$pid is no longer present" >&2
		fi
	done
}

[ "$(id -u)" = 0 ] || b2_skip "must run as root in Linux 5.10.29 guest"
case "$(uname -r)" in
5.10.*) ;;
*) b2_skip "expected Linux 5.10.x guest, got $(uname -r)" ;;
esac

B2_TMP=$(mktemp -d /tmp/b2-process-tree.XXXXXX)
export B2_TMP
cleanup() {
	if [ -n "${ROOT_PID:-}" ] && [ "$ROOT_PID" -gt 0 ] 2>/dev/null; then
		kill -9 "$ROOT_PID" 2>/dev/null || true
	fi
	if [ -n "${CHILD_PID:-}" ] && [ "$CHILD_PID" -gt 0 ] 2>/dev/null; then
		kill -9 "$CHILD_PID" 2>/dev/null || true
	fi
	wait "$ROOT_PID" 2>/dev/null || true
	rm -rf "$B2_TMP"
}
trap cleanup EXIT HUP INT TERM

FIXTURE="$ROOT/tests/progs/tree-session"
RESTORE="$ROOT/userspace/mini-restore/mini-restore"
CRIU=${CRIU:-$(command -v criu 2>/dev/null || true)}
[ -x "$FIXTURE" ] || b2_fail "fixture not built: $FIXTURE"
[ -x "$RESTORE" ] || b2_fail "mini-restore not built: $RESTORE"
[ -n "$CRIU" ] || [ -x "$ROOT/criu/criu/criu" ] ||
	b2_skip "criu unavailable in guest"
[ -n "$CRIU" ] || CRIU="$ROOT/criu/criu/criu"

OUT="$B2_TMP/fixture.out"
IMAGES="$B2_TMP/images"
mkdir -p "$IMAGES"
DMESG_MARK=$(b1_dmesg_mark)
(cd /tmp && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
	setsid "$FIXTURE" >"$OUT" 2>"$B2_TMP/fixture.err") &
ROOT_PID=$!
for _ in $(seq 1 100); do
	grep -q '^READY ' "$OUT" 2>/dev/null && break
	sleep 0.05
done
grep -q '^READY ' "$OUT" || b2_fail "tree fixture did not start"
ROOT_PID=$(sed -n 's/^READY root=\([0-9][0-9]*\) child=.*/\1/p' "$OUT" |
	head -n 1)
CHILD_PID=$(sed -n 's/^READY root=[0-9][0-9]* child=\([0-9][0-9]*\).*/\1/p' "$OUT" |
	head -n 1)
[ -n "$ROOT_PID" ] && [ -n "$CHILD_PID" ] ||
	b2_fail "tree fixture did not report both PIDs"

stat_line() {
	awk '{print $4, $5, $6}' "/proc/$1/stat"
}
before_root=$(stat_line "$ROOT_PID")
before_child=$(stat_line "$CHILD_PID")

if ! "$CRIU" dump -t "$ROOT_PID" -D "$IMAGES" --shell-job --leave-running \
	-v4 --log-file="$B2_TMP/dump.log"; then
	dump_failure_context
	b2_fail "real CRIU tree dump failed"
fi
kill -9 "$ROOT_PID" "$CHILD_PID" 2>/dev/null || true
wait "$ROOT_PID" 2>/dev/null || true
ROOT_PID=0
CHILD_PID=0

"$RESTORE" --pstree "$IMAGES" --restore-sibling >"$B2_TMP/restore.log" 2>&1 ||
	b2_fail "mini-restore tree path failed: $(cat "$B2_TMP/restore.log")"
grep -Fq 'B2_RESTORE' "$B2_TMP/restore.log" ||
	b2_fail "tree restore emitted no completion marker"

for pid in $(sed -n 's/^READY root=\([0-9][0-9]*\) child=.*/\1/p' "$OUT" |
	head -n 1) $(sed -n 's/^READY root=[0-9][0-9]* child=\([0-9][0-9]*\).*/\1/p' "$OUT" |
	head -n 1); do
	alive=0
	for _ in $(seq 1 100); do
		if kill -0 "$pid" 2>/dev/null && test -r "/proc/$pid/stat"; then
			alive=1
			break
		fi
		sleep 0.05
	done
	if [ "$alive" -ne 1 ]; then
		echo "--- tree restore log ---" >&2
		cat "$B2_TMP/restore.log" >&2 || true
		echo "--- tree restore dmesg ---" >&2
		dmesg | tail -n 160 >&2 || true
		b2_fail "restored PID $pid is not alive"
	fi
done

restored_root=$(sed -n 's/^READY root=\([0-9][0-9]*\) child=.*/\1/p' "$OUT" |
	head -n 1)
restored_child=$(sed -n 's/^READY root=[0-9][0-9]* child=\([0-9][0-9]*\).*/\1/p' "$OUT" |
	head -n 1)
[ "$(stat_line "$restored_root")" = "$before_root" ] ||
	b2_fail "root topology changed: before=$before_root after=$(stat_line "$restored_root")"
[ "$(stat_line "$restored_child")" = "$before_child" ] ||
	b2_fail "child topology changed: before=$before_child after=$(stat_line "$restored_child")"

b1_dmesg_check "$DMESG_MARK"
echo "B2_PROCESS_TREE_RESTORE: PASS root=$restored_root child=$restored_child"
