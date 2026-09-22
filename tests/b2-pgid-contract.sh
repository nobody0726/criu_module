#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b2-pgid.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
cc -O2 -Wall -Wextra -Werror -std=c11 \
	-I"$ROOT/include" -I"$ROOT/userspace/mini-restore" \
	"$ROOT/tests/progs/b2-pgid-probe.c" \
	"$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/probe"
"$TMP/probe" | grep -Fq 'pgid-barrier-ok'
echo "B2_PGID_BARRIER: PASS"
