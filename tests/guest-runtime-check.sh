#!/bin/sh
set -eu

echo "=== runtime mounts ==="
mount | grep -E ' on /(usr|lib) ' || {
	echo "runtime 9p mounts are missing" >&2
	exit 1
}
/usr/bin/python3 --version
/lib/ld-linux-aarch64.so.1 --list /mnt/host/criu/criu/criu
echo "A3_RUNTIME: PASS"
