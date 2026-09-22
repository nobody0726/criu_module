#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b2-construction.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
cc -O2 -Wall -Wextra -Werror -std=c11 \
	-I"$ROOT/include" -I"$ROOT/userspace/mini-restore" \
	"$ROOT/tests/progs/b2-fork-probe.c" \
	"$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/probe"

"$TMP/probe" | grep -Fq 'first=3 order=1000,1001,1002,1003'
echo "B2_PROCESS_TREE_CONSTRUCTION: PASS"
