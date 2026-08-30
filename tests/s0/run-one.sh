#!/bin/sh

set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
RUN_DIR=${1:-}
PROBE=${2:-}
KDIR=${KDIR:-${HOME:-}/kernels/linux-5.10.29}
MANIFEST=$ROOT/tests/s0/probes.tsv
SPIKE_DIR=$ROOT/spike
GUEST_EVIDENCE=$ROOT/tests/s0/.s0-guest

fail()
{
	printf 'S0: %s\n' "$*" >&2
}

[ -n "$RUN_DIR" ] || { fail 'run directory is required'; exit 1; }
[ -n "$PROBE" ] || { fail 'probe id is required'; exit 1; }
[ -d "$RUN_DIR" ] || { fail "run directory does not exist: $RUN_DIR"; exit 1; }

record=$(awk -F '\t' -v wanted="$PROBE" '
!header {
	if (NF == 7 && $1 == "id" && $2 == "group" && $3 == "kind" &&
	    $4 == "symbol" && $5 == "expected" && $6 == "gate" &&
	    $7 == "fixture")
		header = 1
	next
}
$1 == wanted { print; found++ }
END { if (!header || found != 1) exit 1 }
' "$MANIFEST") || {
	fail "probe is missing or duplicated in manifest: $PROBE"
	exit 1
}

tab=$(printf '\t')
IFS="$tab" read -r id group kind symbol expected gate fixture <<EOF
$record
EOF

PROBE_DIR=$RUN_DIR/$PROBE
mkdir -p "$PROBE_DIR" || exit 1
printf '%s\n' "$record" > "$PROBE_DIR/manifest-record.tsv" || exit 1

env_value()
{
	awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); exit }' \
		"$RUN_DIR/environment.txt"
}

KERNEL_RELEASE=$(env_value KERNEL_RELEASE)
ARCH=$(env_value ARCH)
CONFIG_HASH=$(env_value CONFIG_HASH)
[ -n "$KERNEL_RELEASE" ] || KERNEL_RELEASE=unknown
[ -n "$ARCH" ] || ARCH=unknown
[ -n "$CONFIG_HASH" ] || CONFIG_HASH=unknown

write_result()
{
	result_status=$1
	result_reason=$2
	{
		printf 'probe=%s\n' "$PROBE"
		printf 'status=%s\n' "$result_status"
		printf 'expected=%s\n' "$expected"
		printf 'kernel=%s\n' "$KERNEL_RELEASE"
		printf 'arch=%s\n' "$ARCH"
		printf 'config_hash=%s\n' "$CONFIG_HASH"
		printf 'reason=%s\n' "$result_reason"
	} > "$PROBE_DIR/result.txt"
}

clean_status=0
make -C "$SPIKE_DIR" KDIR="$KDIR" clean > "$PROBE_DIR/clean.log" 2>&1 || clean_status=$?
if [ "$clean_status" -ne 0 ]; then
	write_result CRASH "spike clean failed with status $clean_status"
	exit 1
fi

if [ "$kind" = compile ]; then
	build_status=0
	make -C "$SPIKE_DIR" KDIR="$KDIR" PROBE="$PROBE" \
		> "$PROBE_DIR/build.log" 2>&1 || build_status=$?
	if [ "$build_status" -eq 0 ]; then
		if [ "$expected" = linkable-current-mm ]; then
			write_result OK 'symbol links; wrapper scope is current->mm'
		else
			write_result OK 'compile probe linked successfully'
		fi
		cp "$PROBE_DIR/build.log" "$PROBE_DIR/modpost.log" 2>/dev/null || true
		exit 0
	fi
	if grep -Eq 'modpost: "[^" ]+" .* undefined!' "$PROBE_DIR/build.log"; then
		grep -E 'modpost: "[^" ]+" .* undefined!' "$PROBE_DIR/build.log" \
			> "$PROBE_DIR/modpost.log" || true
		write_result NO-SYMBOL 'modpost reported an unresolved kernel symbol'
		exit 0
	fi
	write_result CRASH "compile failed with status $build_status"
	exit 1
fi

if [ "$kind" != runtime ]; then
	write_result ENV-MISMATCH "unsupported probe kind: $kind"
	exit 1
fi

build_status=0
make -C "$SPIKE_DIR" KDIR="$KDIR" PROBE="$PROBE" \
	> "$PROBE_DIR/build.log" 2>&1 || build_status=$?
if [ "$build_status" -ne 0 ]; then
	write_result CRASH "runtime module build failed with status $build_status"
	exit 1
fi

printf '%s\n' "$PROBE" > "$ROOT/tests/s0/.s0-probe" || {
	write_result CRASH 'cannot select guest probe'
	exit 1
}

mkdir -p "$GUEST_EVIDENCE" || {
	write_result CRASH 'cannot create guest staging directory'
	exit 1
}
for evidence in \
	status report vmas fixture.out fixture-comm fixture-tgid \
	maps smaps maps-before maps-after smaps-before smaps-after smaps-delta \
	dmesg-before dmesg-after rmmod.error
do
	rm -f "$GUEST_EVIDENCE/$evidence"
done

qemu_status=0
if command -v timeout >/dev/null 2>&1; then
	timeout --foreground 180 "$ROOT/scripts/run-qemu.sh" --ci \
		--script tests/s0/guest-run-one.sh > "$PROBE_DIR/qemu.log" 2>&1 || qemu_status=$?
else
	"$ROOT/scripts/run-qemu.sh" --ci --script tests/s0/guest-run-one.sh \
		> "$PROBE_DIR/qemu.log" 2>&1 || qemu_status=$?
fi

for evidence in \
	status report vmas fixture.out fixture-comm fixture-tgid \
	maps smaps maps-before maps-after smaps-before smaps-after smaps-delta \
	dmesg-before dmesg-after rmmod.error
do
	if [ -e "$GUEST_EVIDENCE/$evidence" ]; then
		cp "$GUEST_EVIDENCE/$evidence" "$PROBE_DIR/$evidence" 2>/dev/null || true
	fi
done
if [ -e "$PROBE_DIR/maps-before" ]; then
	cp "$PROBE_DIR/maps-before" "$PROBE_DIR/proc-maps"
fi
if [ -e "$PROBE_DIR/smaps-before" ]; then
	cp "$PROBE_DIR/smaps-before" "$PROBE_DIR/proc-smaps"
fi

marker=$(grep -E "^S0_RESULT: ${PROBE}:" "$PROBE_DIR/qemu.log" | tail -n 1 || true)
if [ -n "$marker" ]; then
	observed=$(printf '%s\n' "${marker##*:}" | tr -d '\r')
	case "$observed" in
		OK|UNSAFE|NO-SYMBOL|WRONG-VALUE|CRASH|CLEANUP-FAIL|ENV-MISMATCH)
			write_result "$observed" 'guest protocol reported the probe result'
			;;
		*)
			write_result CRASH "guest returned an invalid result marker: $observed"
			qemu_status=1
			;;
	esac
else
	if [ -s "$PROBE_DIR/rmmod.error" ]; then
		write_result CLEANUP-FAIL 'QEMU exited without a result and rmmod reported an error'
	else
		write_result CRASH "QEMU exited without S0_RESULT (status $qemu_status)"
	fi
	qemu_status=1
fi

rm -f "$ROOT/tests/s0/.s0-probe"
exit "$qemu_status"
