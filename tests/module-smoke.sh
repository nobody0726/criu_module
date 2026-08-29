#!/bin/sh
set -eu

MODULE=${MODULE:-./kernel_module/criu_probe.ko}
DEBUG_ROOT=${DEBUG_ROOT:-/sys/kernel/debug}
DEBUG_DIR="$DEBUG_ROOT/criu_probe"

fail()
{
	echo "MODULE_SMOKE: FAIL: $*" >&2
	exit 1
}

[ "$(id -u)" = 0 ] || fail "must run as root inside the target guest"
[ -f "$MODULE" ] || fail "module not found: $MODULE"

mount -t debugfs none "$DEBUG_ROOT" 2>/dev/null || true

insmod "$MODULE" || fail "insmod failed"
trap 'rmmod criu_probe 2>/dev/null || true' EXIT

[ -r "$DEBUG_DIR/status" ] || fail "status file missing"
status=$(cat "$DEBUG_DIR/status")
[ "$status" = "criu_probe:ok" ] || fail "unexpected status: $status"

dmesg | tail -n 80 | grep -q 'criu_probe: loaded' \
	|| fail "load message missing"

rmmod criu_probe || fail "rmmod failed"
trap - EXIT
[ ! -e "$DEBUG_DIR/status" ] || fail "status file survived unload"

echo "MODULE_SMOKE: PASS"
