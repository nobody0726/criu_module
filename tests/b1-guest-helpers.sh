#!/bin/sh
set -eu

b1_fail() {
	echo "B1_KERNEL_ASSISTED_RESTORE: FAIL: $*" >&2
	exit 1
}

b1_skip() {
	echo "B1_KERNEL_ASSISTED_RESTORE: SKIP: $*" >&2
	exit 77
}

b1_require_guest_tmp() {
	case "${B1_TMP:-}" in
	/tmp/*) ;;
	*) b1_fail "B1_TMP must be guest-local /tmp, got ${B1_TMP:-unset}" ;;
	esac
}

b1_dmesg_mark() {
	dmesg | wc -l
}

b1_dmesg_check() {
	mark=$1
	if dmesg | tail -n +"$((mark + 1))" | grep -E 'BUG:|WARNING:|Oops|KASAN|refcount|use-after-free' >&2; then
		b1_fail "dirty dmesg"
	fi
}
