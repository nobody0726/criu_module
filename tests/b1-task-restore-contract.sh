#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/b1-task-restore.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/probe.c" <<'EOF'
#include "task_restore.h"

int main(void)
{
	struct b2_task_restore task;

	b1_task_restore_init(&task);
	b1_task_restore_destroy(&task);
	return 0;
}
EOF

make -C "$ROOT/userspace/mini-restore" clean all >/dev/null
cc -O2 -Wall -Wextra -Werror -std=c11 \
	-I"$ROOT/include" -I"$ROOT/userspace/mini-restore" \
	"$TMP/probe.c" "$ROOT/userspace/mini-restore/libmini_restore.a" \
	-o "$TMP/probe"
"$TMP/probe"

echo "B1_TASK_RESTORE_CONTRACT: PASS"
