#!/bin/sh
set +e
echo "DIAG uid=$(id -u)"
echo "DIAG filesystems"
cat /proc/filesystems
echo "DIAG mounts"
cat /proc/mounts
echo "DIAG dir"
ls -ld /sys /sys/kernel /sys/kernel/debug
mkdir -p /sys/kernel/debug
mount -t debugfs none /sys/kernel/debug
echo "DIAG mount_rc=$?"
echo "DIAG mounts_after"
cat /proc/mounts
exit 0
