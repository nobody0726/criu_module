#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b2-shared.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
cc -O2 -Wall -Wextra -Werror -std=c11 \
	-I"$ROOT/userspace/mini-restore" \
	"$ROOT/tests/progs/b2-shared-probe.c" \
	"$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/probe"
"$TMP/probe" | grep -Fq 'shared-ok'
echo "B2_SHARED_CONTRACT: PASS"
