#!/usr/bin/env bash
set -euo pipefail

[ "$(uname -s)" = Linux ] || {
	echo 'A5_BEHAVIOR: SKIP (Linux fixtures; run in Lima)'
	exit 77
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CC_BIN=${CC:-cc}
ulimit -n 4096
TMP=$(mktemp -d "${TMPDIR:-/tmp}/a5-behavior.XXXXXX")
pid=0
cleanup() {
	if [ "$pid" -gt 0 ]; then kill -9 "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT

compile_and_check()
{
	name=$1
	"$CC_BIN" -std=gnu11 -O2 -Wall -Wextra -Werror \
		"$ROOT/tests/progs/$name.c" -o "$TMP/$name"
	: >"$TMP/$name.in"
	A5_REG_PATH="$TMP/shared" "$TMP/$name" <"$TMP/$name.in" >"$TMP/$name.out" 2>"$TMP/$name.err" &
	pid=$!
	for _ in $(seq 1 100); do
		grep -q '^pid=' "$TMP/$name.out" 2>/dev/null && break
		sleep 0.01
	done
	grep -q '^pid=' "$TMP/$name.out" || {
		cat "$TMP/$name.err" >&2
		return 1
	}
	printf x >>"$TMP/$name.in"
	for _ in $(seq 1 100); do
		grep -q -- '-check=PASS' "$TMP/$name.out" 2>/dev/null && break
		sleep 0.01
	done
	grep -q -- '-check=PASS' "$TMP/$name.out" || {
		cat "$TMP/$name.out" >&2
		kill -9 "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
		return 1
	}
	printf x >>"$TMP/$name.in"
	for _ in $(seq 1 100); do
		grep -q '^alive=1$' "$TMP/$name.out" && break
		sleep 0.01
	done
	grep -q '^alive=1$' "$TMP/$name.out"
	kill -9 "$pid"
	wait "$pid" 2>/dev/null || true
	pid=0
}

compile_and_check fds-dup
compile_and_check fds-pipe
compile_and_check fds-unix-stream

echo 'A5_BEHAVIOR: PASS'
