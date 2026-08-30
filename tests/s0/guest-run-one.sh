#!/bin/sh

set -u

ROOT=/mnt/host
DEBUG_ROOT=/sys/kernel/debug
DEBUG_DIR=$DEBUG_ROOT/criu_spike
PROBE_FILE=$ROOT/tests/s0/.s0-probe
MANIFEST=$ROOT/tests/s0/probes.tsv
OUT_DIR=${S0_OUT_DIR:-$ROOT/tests/s0/.s0-guest}
PROBE=${S0_PROBE:-}
TARGET_PID=
MODULE_LOADED=0
CLEANED=0
RESULT_EMITTED=0
DMESG_LINES=0
FINAL_STATUS=CRASH
FINAL_RC=1
FIXTURE_STARTED=0
FIXTURE_REAPED=0
RMMOD_ERROR=

emit_result()
{
	[ "$RESULT_EMITTED" -eq 0 ] || return 0
	echo "S0_RESULT: ${PROBE:-unknown}:${1}"
	RESULT_EMITTED=1
}

fail()
{
	if [ "$#" -eq 1 ]; then
		FINAL_STATUS=CRASH
	else
		FINAL_STATUS=$1
		shift
	fi
	echo "S0: $*" >&2
	exit 1
}

cleanup()
{
	[ "$CLEANED" -eq 0 ] || return 0
	CLEANED=1
	cleanup_failed=0

	if [ "$MODULE_LOADED" -eq 1 ]; then
		RMMOD_ERROR=$OUT_DIR/rmmod.error
		if rmmod criu_spike >"$RMMOD_ERROR" 2>&1; then
			MODULE_LOADED=0
		else
			cleanup_failed=1
		fi
		[ "$MODULE_LOADED" -eq 1 ] || [ ! -e "$DEBUG_DIR/status" ] || \
			cleanup_failed=1
	fi
	if [ "$FIXTURE_STARTED" -eq 1 ] && [ "$FIXTURE_REAPED" -eq 0 ]; then
		if kill -0 "$TARGET_PID" 2>/dev/null; then
			kill "$TARGET_PID" 2>/dev/null || cleanup_failed=1
		fi
		wait "$TARGET_PID" 2>/dev/null || true
		FIXTURE_REAPED=1
	fi
	return "$cleanup_failed"
}

check_dmesg_after_cleanup()
{
	[ -r "$OUT_DIR/dmesg-before" ] || return 0
	if ! dmesg > "$OUT_DIR/dmesg-after"; then
		echo "S0: cannot save post-cleanup dmesg" >&2
		return 1
	fi
	new_dmesg=$(dmesg | tail -n +$((DMESG_LINES + 1)) | grep -E \
		'BUG:|WARNING:|Oops|panic|KASAN|use-after-free|possible circular locking|sleeping function called|suspicious RCU usage' || true)
	if [ -n "$new_dmesg" ]; then
		echo "$new_dmesg" >&2
		return 1
	fi
	return 0
}

finish()
{
	cleanup_status=0
	dmesg_status=0
	cleanup || cleanup_status=1
	check_dmesg_after_cleanup || dmesg_status=1
	if [ "$cleanup_status" -ne 0 ]; then
		FINAL_STATUS=CLEANUP-FAIL
	elif [ "$dmesg_status" -ne 0 ]; then
		FINAL_STATUS=CRASH
	fi
	emit_result "$FINAL_STATUS"
	exit "$FINAL_RC"
}

trap finish 0 1 2 3 15

[ "$(id -u)" = 0 ] || fail ENV-MISMATCH "must run as root inside QEMU guest"
[ "$(uname -s)" = Linux ] || fail ENV-MISMATCH "guest kernel is not Linux"
KERNEL_RELEASE=$(uname -r)
case "$KERNEL_RELEASE" in
	5.10.[0-9]*) ;;
	*) fail ENV-MISMATCH "requires Linux 5.10.x, got $KERNEL_RELEASE" ;;
esac

[ -d "$ROOT" ] || fail ENV-MISMATCH "project mount is missing: $ROOT"
[ -r "$MANIFEST" ] || fail ENV-MISMATCH "probe manifest is missing: $MANIFEST"

if [ -z "$PROBE" ] && [ -r "$PROBE_FILE" ]; then
	PROBE=$(tr -d '[:space:]' < "$PROBE_FILE")
fi
[ -n "$PROBE" ] || fail ENV-MISMATCH "selected probe is missing; set S0_PROBE or create $PROBE_FILE"

if ! manifest_record=$(awk -F '\t' -v wanted="$PROBE" '
BEGIN {
	spec["1.1"] = "1\tcompile\tfind_get_task_by_vpid\tNO-SYMBOL\tno\tnone"
	spec["1.2"] = "1\truntime\tpid_task/find_vpid\tOK\tyes\tpid"
	spec["1.3"] = "1\truntime\tget_pid_task/put_task_struct\tOK\tyes\tpid"
	spec["1.4"] = "1\truntime\tpid_task/find_vpid (without RCU)\tUNSAFE\tno\tpid"
	spec["2.1"] = "2\tcompile\tmmget_not_zero\tNO-SYMBOL\tno\tnone"
	spec["2.2"] = "2\truntime\tget_task_mm/mmput\tOK\tyes\tpid"
	spec["2.3"] = "2\truntime\tmmap_read_lock\tOK\tyes\tpid"
	spec["2.4"] = "2\truntime\tmm->mmap/vm_next\tOK\tyes\tpid,anon,shared"
	spec["2.5"] = "2\truntime\tvm_file/d_path\tOK\tno\tpid"
	spec["3.1"] = "3\tcompile\tfollow_page\tNO-SYMBOL\tno\tnone"
	spec["3.2"] = "3\truntime\tget_user_pages_remote\tOK\tyes\tpid,anon"
	spec["3.3"] = "3\truntime\taccess_process_vm\tOK\tyes\tpid,anon,shared"
	spec["3.4"] = "3\truntime\tunmapped address\tOK\tno\tpid"
	spec["3.5"] = "3\truntime\tPROT_NONE guard page\tOK\tno\tpid,guard"
	spec["4.1"] = "4\tcompile\tmm_alloc\tNO-SYMBOL\tno\tnone"
	spec["4.2"] = "4\tcompile\tvm_area_alloc\tNO-SYMBOL\tno\tnone"
	spec["4.3"] = "4\tcompile\tinsert_vm_struct\tNO-SYMBOL\tno\tnone"
	spec["4.4a"] = "4\tcompile\tdo_mmap\tNO-SYMBOL\tno\tnone"
	spec["4.4b"] = "4\tcompile\tvm_mmap\tlinkable-current-mm\tno\tnone"
	spec["4.5a"] = "4\tcompile\tdo_munmap\tNO-SYMBOL\tno\tnone"
	spec["4.5b"] = "4\tcompile\tvm_munmap\tlinkable-current-mm\tno\tnone"
	spec["4.6"] = "4\tcompile\talloc_pid\tNO-SYMBOL\tno\tnone"
	spec["4.7"] = "4\tcompile\tkernel_execve\tNO-SYMBOL\tno\tnone"
	spec["4.8"] = "4\truntime\tvm_insert_page\tUNSAFE\tno\tpid,insert"
}
/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
!header {
	if (NF != 7 || $1 != "id" || $2 != "group" || $3 != "kind" ||
	    $4 != "symbol" || $5 != "expected" || $6 != "gate" || $7 != "fixture")
		bad = 1
	header = 1
	next
}
{
	row = $2 FS $3 FS $4 FS $5 FS $6 FS $7
	if (NF != 7 || !($1 in spec) || seen[$1]++ || row != spec[$1])
		bad = 1
	count++
	if ($1 == wanted) {
		matches++
		record = $0
	}
}
END {
	if (!header || bad || count != 24 || matches != 1)
		exit 1
	print record
}' "$MANIFEST"); then
	fail ENV-MISMATCH "invalid probe manifest"
fi

[ -x "$ROOT/tests/progs/known-layout" ] || \
	fail ENV-MISMATCH "known-layout fixture is missing or not executable"
if command -v file >/dev/null 2>&1; then
	file_output=$(file "$ROOT/tests/progs/known-layout")
	printf '%s\n' "$file_output" | grep -qi 'statically linked' || \
		fail ENV-MISMATCH "known-layout is not static: $file_output"
elif command -v readelf >/dev/null 2>&1; then
	readelf -l "$ROOT/tests/progs/known-layout" | grep -q 'INTERP' && \
		fail ENV-MISMATCH "known-layout has a dynamic interpreter"
fi

[ -r "$ROOT/spike/criu_spike.ko" ] || fail ENV-MISMATCH "module is missing or unreadable"

[ -d "$DEBUG_ROOT" ] || mkdir -p "$DEBUG_ROOT" || \
	fail ENV-MISMATCH "cannot create debugfs mount point"
if ! grep -q "[[:space:]]$DEBUG_ROOT[[:space:]]" /proc/mounts; then
	mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null || \
		fail ENV-MISMATCH "cannot mount debugfs"
fi
[ -d "$DEBUG_ROOT" ] || fail ENV-MISMATCH "debugfs root is unavailable"

mkdir -p "$OUT_DIR" || fail "cannot create guest evidence directory"
rm -f "$OUT_DIR"/maps "$OUT_DIR"/smaps "$OUT_DIR"/maps-before \
	"$OUT_DIR"/smaps-before "$OUT_DIR"/maps-after "$OUT_DIR"/smaps-after \
	"$OUT_DIR"/dmesg-before "$OUT_DIR"/dmesg-after "$OUT_DIR"/status \
	"$OUT_DIR"/report "$OUT_DIR"/vmas "$OUT_DIR"/fixture.out \
	"$OUT_DIR"/rmmod.error || fail CRASH "cannot reset evidence directory"

"$ROOT/tests/progs/known-layout" > "$OUT_DIR/fixture.out" 2>&1 &
TARGET_PID=$!
FIXTURE_STARTED=1

i=0
while [ "$i" -lt 20 ]; do
	if grep -q '^pid=' "$OUT_DIR/fixture.out"; then
		break
	fi
	sleep 1
	i=$((i + 1))
done

fixture_line=$(sed -n '1p' "$OUT_DIR/fixture.out")
set -- $fixture_line
[ "$#" -eq 5 ] || fail CRASH "cannot parse fixture output"
case "$1" in
pid=*) TARGET_PID=${1#pid=} ;;
*) fail CRASH "invalid fixture pid" ;;
esac
case "$2" in
anon=*) ANON_ADDR=${2#anon=} ;;
*) fail CRASH "invalid anon address" ;;
esac
case "$3" in
shared=*) SHARED_ADDR=${3#shared=} ;;
*) fail CRASH "invalid shared address" ;;
esac
case "$4" in
guard=*) GUARD_ADDR=${4#guard=} ;;
*) fail CRASH "invalid guard address" ;;
esac
case "$5" in
insert=*) INSERT_ADDR=${5#insert=} ;;
*) fail CRASH "invalid insert address" ;;
esac
case "$TARGET_PID" in ''|0|*[!0-9]*) fail CRASH "invalid fixture pid" ;; esac
case "$ANON_ADDR:$SHARED_ADDR:$GUARD_ADDR:$INSERT_ADDR" in
	0x[0-9a-fA-F][0-9a-fA-F]*:0x[0-9a-fA-F][0-9a-fA-F]*:0x[0-9a-fA-F][0-9a-fA-F]*:0x[0-9a-fA-F][0-9a-fA-F]*) ;;
	*) fail CRASH "invalid fixture address" ;;
esac

kill -0 "$TARGET_PID" 2>/dev/null || fail CRASH "fixture exited before probe"

cat "/proc/$TARGET_PID/maps" > "$OUT_DIR/maps-before" || fail CRASH "cannot save proc maps"
cat "/proc/$TARGET_PID/smaps" > "$OUT_DIR/smaps-before" || fail CRASH "cannot save proc smaps"
cp "$OUT_DIR/maps-before" "$OUT_DIR/maps" || fail CRASH "cannot save maps evidence"
cp "$OUT_DIR/smaps-before" "$OUT_DIR/smaps" || fail CRASH "cannot save smaps evidence"
dmesg > "$OUT_DIR/dmesg-before" || fail CRASH "cannot save dmesg baseline"
DMESG_LINES=$(wc -l < "$OUT_DIR/dmesg-before")

insmod "$ROOT/spike/criu_spike.ko" \
	target_pid="$TARGET_PID" anon_addr="$ANON_ADDR" \
	shared_addr="$SHARED_ADDR" guard_addr="$GUARD_ADDR" \
	insert_addr="$INSERT_ADDR" probe="$PROBE" || fail CRASH "insmod failed"
MODULE_LOADED=1

[ -r "$DEBUG_DIR/status" ] || fail CRASH "status file is missing"
[ -r "$DEBUG_DIR/report" ] || fail CRASH "report file is missing"
[ -r "$DEBUG_DIR/vmas" ] || fail CRASH "vma report is missing"
cat "$DEBUG_DIR/status" > "$OUT_DIR/status" || fail CRASH "cannot save status"
cat "$DEBUG_DIR/report" > "$OUT_DIR/report" || fail CRASH "cannot save report"
cat "$DEBUG_DIR/vmas" > "$OUT_DIR/vmas" || fail CRASH "cannot save vma report"
[ "$(wc -c < "$OUT_DIR/status")" -le 4096 ] || fail CRASH "status output is unbounded"
[ "$(wc -c < "$OUT_DIR/report")" -le 4096 ] || fail CRASH "report output is unbounded"
[ "$(wc -c < "$OUT_DIR/vmas")" -le 4096 ] || fail CRASH "vma output is unbounded"
grep -q '^protocol=1$' "$OUT_DIR/status" || fail CRASH "status is incomplete"
grep -q '^state=READY$' "$OUT_DIR/status" || fail CRASH "status is incomplete"
grep -q '^result=NOT_IMPLEMENTED$' "$OUT_DIR/status" || fail CRASH "status is incomplete"
grep -q "^probe=$PROBE$" "$OUT_DIR/report" || fail CRASH "report probe is incomplete"
grep -q "^target_pid=$TARGET_PID$" "$OUT_DIR/report" || fail CRASH "report pid is incomplete"
grep -q "^anon_addr=$ANON_ADDR$" "$OUT_DIR/report" || fail CRASH "report anon address is incomplete"
grep -q "^shared_addr=$SHARED_ADDR$" "$OUT_DIR/report" || fail CRASH "report shared address is incomplete"
grep -q "^guard_addr=$GUARD_ADDR$" "$OUT_DIR/report" || fail CRASH "report guard address is incomplete"
grep -q "^insert_addr=$INSERT_ADDR$" "$OUT_DIR/report" || fail CRASH "report insert address is incomplete"
grep -q '^target_valid=1$' "$OUT_DIR/report" || fail CRASH "report target validation is incomplete"
grep -q '^result=NOT_IMPLEMENTED$' "$OUT_DIR/report" || fail CRASH "framework report is incomplete"
grep -q '^protocol=1$' "$OUT_DIR/vmas" || fail CRASH "vma report is incomplete"
grep -q "^probe=$PROBE$" "$OUT_DIR/vmas" || fail CRASH "vma report probe is incomplete"
grep -q '^result=NOT_IMPLEMENTED$' "$OUT_DIR/vmas" || fail CRASH "vma report is incomplete"
grep -q '^vmas=NOT_IMPLEMENTED$' "$OUT_DIR/vmas" || fail CRASH "vma report is incomplete"

cat "/proc/$TARGET_PID/maps" > "$OUT_DIR/maps-after" || fail CRASH "cannot save post-probe maps"
cat "/proc/$TARGET_PID/smaps" > "$OUT_DIR/smaps-after" || fail CRASH "cannot save post-probe smaps"

# Task 3 deliberately has no concrete probe. Never serialize this placeholder
# as a result accepted by the S0 matrix; the orchestrator must classify it as a
# failed/incomplete probe until a later task supplies a real result.
echo "S0: selected probe is not implemented" >&2
FINAL_STATUS=CRASH
FINAL_RC=1
exit "$FINAL_RC"
