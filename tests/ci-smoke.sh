#!/bin/sh
# ci-smoke.sh -- the in-guest gate. Runs inside the QEMU/virtme guest as root,
# invoked by scripts/run-qemu.sh --ci --script tests/ci-smoke.sh.
#
# Contract with .github/workflows/qemu-test.yml:
#   the LAST line on success is exactly "CI_RESULT: PASS".
#   Anything else (including a guest panic that kills this script) is a failure,
#   because the workflow greps for that marker rather than trusting an exit code
#   -- a guest that dies mid-test cannot return one.
#
# Nothing here is allowed to run on the developer's host: see docs/04-Dev-Environment.md
# section on L1/L2/L3. A bad vma->vm_next dereference takes the whole kernel down.
set -eu

fail() {
	echo "SMOKE: $*" >&2
	echo "CI_RESULT: FAIL"
	exit 1
}

# ---------------------------------------------------------------------------
# Guest sanity: this script is worthless outside the disposable VM.
# ---------------------------------------------------------------------------
[ "$(id -u)" = "0" ] || fail "must run as root inside the guest"
[ -d /sys/kernel/debug ] || mkdir -p /sys/kernel/debug
mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug \
	|| fail "cannot mount debugfs"

KVER=$(uname -r)
echo "SMOKE: kernel $KVER"
case "$KVER" in
5.10.*) ;;
*)	# Not fatal by itself, but every VMA walk in this project assumes
	# vma->vm_next, which 6.1 removed. Loud notice beats a silent misread.
	echo "SMOKE: WARNING: expected 5.10.x, got $KVER" ;;
esac

MOD=/mnt/host/kernel_module/criu_kernel.ko
[ -f "$MOD" ] || MOD=./kernel_module/criu_kernel.ko

# ---------------------------------------------------------------------------
# dmesg hygiene. Checked after every gate, not just at the end: a lockdep splat
# needs to be attributed to the gate that produced it.
# ---------------------------------------------------------------------------
DMESG_MARK=0
dmesg_check() {
	_what="$1"
	_out=$(dmesg | tail -n +$((DMESG_MARK + 1)) | grep -nE \
		'BUG:|WARNING:|Oops|general protection|kasan|KASAN|use-after-free|possible circular locking|sleeping function called|suspicious RCU usage' \
		|| true)
	if [ -n "$_out" ]; then
		echo "SMOKE: dmesg is dirty after $_what:" >&2
		echo "$_out" >&2
		# Print surrounding context: the first line of a splat is rarely
		# the informative one.
		dmesg | tail -n 60 >&2
		fail "dirty dmesg after $_what"
	fi
	DMESG_MARK=$(dmesg | wc -l)
	echo "SMOKE: dmesg clean after $_what"
}

DMESG_MARK=$(dmesg | wc -l)

# ---------------------------------------------------------------------------
# Gate 0: the module loads and unloads at all. Every later gate assumes this,
# so failing here should say so plainly rather than surfacing as ten failures.
# ---------------------------------------------------------------------------
if [ -f "$MOD" ]; then
	insmod "$MOD" || fail "insmod $MOD"
	[ -d /sys/kernel/debug/criu ] || fail "module loaded but /sys/kernel/debug/criu missing"
	rmmod criu_kernel || fail "rmmod criu_kernel"
	dmesg_check "load/unload"
else
	# Legitimate state before A1 lands: the workflow still exercises the
	# kernel build, QEMU boot and script plumbing.
	echo "SMOKE: no module at $MOD yet (pre-A1); skipping module gates"
fi

# ---------------------------------------------------------------------------
# Comparison gates. Each step appends exactly one line here, and each script
# is responsible for its own insmod/rmmod so it can also be run standalone
# during development.
#
#   A1 -> tests/compare/diff-maps.sh
#   A2 -> tests/compare/freeze-test.sh
#   A3 -> tests/compare/cross-restore.sh
#
# A listed-but-missing script is a hard failure, not a skip: a gate that can
# vanish silently is not a gate.
# ---------------------------------------------------------------------------
GATES=""

for g in $GATES; do
	[ -f "$g" ] || fail "gate script $g is listed but missing"
	echo "SMOKE: === $g ==="
	sh "$g" || fail "$g"
	dmesg_check "$g"
done

[ -n "$GATES" ] || echo "SMOKE: no comparison gates registered yet"

# ---------------------------------------------------------------------------
# Slab leak check: a dump that leaks one allocation per VMA is invisible in a
# functional test and fatal in a long-running one.
# ---------------------------------------------------------------------------
if [ -f "$MOD" ] && [ -r /proc/slabinfo ]; then
	before=$(awk '/^kmalloc-/ {s+=$3} END {print s+0}' /proc/slabinfo)
	insmod "$MOD" || fail "insmod for leak check"
	rmmod criu_kernel || fail "rmmod for leak check"
	after=$(awk '/^kmalloc-/ {s+=$3} END {print s+0}' /proc/slabinfo)
	echo "SMOKE: kmalloc objects before=$before after=$after"
	# Deliberately a report, not a gate: unrelated kernel activity moves
	# these counters. A3's completion criteria call for a manual comparison.
	dmesg_check "leak check"
fi

echo "SMOKE: all gates passed"
echo "CI_RESULT: PASS"
