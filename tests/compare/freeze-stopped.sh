#!/bin/sh
set -eu

ROOT=/sys/kernel/debug/criu/
[ -e "${ROOT}freeze" ] || {
	echo "A2_FREEZE: missing freeze control" >&2
	exit 1
}
[ -e "${ROOT}thaw" ] || {
	echo "A2_FREEZE: missing thaw control" >&2
	exit 1
}

echo "A2_FREEZE: stopped-state fixture gate"
