#!/bin/sh
set -eu

ROOT=/sys/kernel/debug/criu/

[ "$(uname -r)" = 5.10.29 ] || {
	echo "A2_FREEZE: wrong guest kernel" >&2
	exit 1
}
[ "$(uname -m)" = aarch64 ] || exit 1
[ "$(id -u)" = 0 ] || exit 1

mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
insmod ./kernel_module/criu_kernel.ko
target_pid=0
cleanup()
{
	[ -e "${ROOT}thaw" ] && printf '1\n' > "${ROOT}thaw" 2>/dev/null || true
	if [ "$target_pid" -gt 0 ]; then
		kill "$target_pid" 2>/dev/null || true
		wait "$target_pid" 2>/dev/null || true
	fi
	rmmod criu_kernel 2>/dev/null || true
}
trap cleanup EXIT

[ -e "${ROOT}freeze" ] || {
	echo "A2_FREEZE: missing freeze control" >&2
	exit 1
}
[ -e "${ROOT}thaw" ] || {
	echo "A2_FREEZE: missing thaw control" >&2
	exit 1
}

write_expect_error()
{
	control=$1
	value=$2
	needle=$3
	error_file=$(mktemp)
	if printf '%s\n' "$value" | dd of="${ROOT}${control}" status=none 2>"$error_file"; then
		rm -f "$error_file"
		echo "A2_FREEZE: ${control} '$value' unexpectedly succeeded" >&2
		exit 1
	fi
	if ! grep -qi "$needle" "$error_file"; then
		cat "$error_file" >&2
		rm -f "$error_file"
		echo "A2_FREEZE: expected '$needle' for ${control} '$value'" >&2
		exit 1
	fi
	rm -f "$error_file"
}

write_freeze_expect_error()
{
	value=$1
	needle=$2
	write_expect_error freeze "$value" "$needle"
}

write_target()
{
	printf '%s\n' "$1" | dd of="${ROOT}target" status=none
}

sleep 1000 &
target_pid=$!

# No selected target must fail without creating a context.
write_freeze_expect_error 1 "No such process"

write_target "$target_pid"
generation=$(sed -n 's/.*generation=\([0-9][0-9]*\).*/\1/p' "${ROOT}target")
[ -n "$generation" ] || {
	echo "A2_FREEZE: missing target generation" >&2
	exit 1
}

printf '1\n' | dd of="${ROOT}freeze" status=none
write_freeze_expect_error 1 "Device or resource busy"
after_generation=$(sed -n 's/.*generation=\([0-9][0-9]*\).*/\1/p' "${ROOT}target")
[ "$after_generation" = "$generation" ] || {
	echo "A2_FREEZE: duplicate freeze changed generation" >&2
	exit 1
}

write_expect_error target "$$" "Device or resource busy"
after_target=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' "${ROOT}target")
[ "$after_target" = "$target_pid" ] || {
	echo "A2_FREEZE: target changed while context exists" >&2
	exit 1
}

printf '1\n' | dd of="${ROOT}thaw" status=none
cat "${ROOT}status" | grep -q 'freeze_state=idle'
echo "A2_FREEZE: PASS (missing target, duplicate freeze, target lock)"
