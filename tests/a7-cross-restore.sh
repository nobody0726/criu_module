#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
	echo "A7_CROSS_RESTORE: SKIP: ENVIRONMENT (nested Linux guest required)"
	exit 0
fi
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
	echo "A7_CROSS_RESTORE: SKIP: ENVIRONMENT (root guest required)"
	exit 0
fi
if [[ ! -e /sys/kernel/debug/criu/dump-tree ]]; then
	echo "A7_CROSS_RESTORE: SKIP: ENVIRONMENT (dump-tree unavailable)"
	exit 0
fi
if ! command -v criu >/dev/null 2>&1; then
	echo "A7_CROSS_RESTORE: SKIP: ENVIRONMENT (criu unavailable)"
	exit 0
fi

echo "A7_SIMPLE: SKIP: IMPLEMENTATION (guest transaction gate not wired)"
echo "A7_SESSION: SKIP: IMPLEMENTATION (guest transaction gate not wired)"
echo "A7_PGID: SKIP: IMPLEMENTATION (guest transaction gate not wired)"
echo "A7_CROSS_RESTORE: SKIP: IMPLEMENTATION"
