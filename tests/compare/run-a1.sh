#!/bin/sh
set -eu
[ "$(uname -r)" = 5.10.29 ] || { echo 'A1: wrong guest kernel'; exit 1; }
[ "$(uname -m)" = aarch64 ] || exit 1
[ "$(id -u)" = 0 ] || exit 1
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
insmod ./kernel_module/criu_kernel.ko
trap 'rmmod criu_kernel' EXIT
./tests/progs/a1-check "$1"
rmmod criu_kernel
trap - EXIT
[ ! -e /sys/kernel/debug/criu ]
