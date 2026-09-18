#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
sh "$ROOT/tests/cross-restore.sh"
sh "$ROOT/tests/a4-cross-restore.sh"
if dmesg | grep -E 'BUG:|WARNING:|Oops:|scheduling while atomic|possible circular locking'; then exit 1; fi
echo 'A5_REGRESSION: PASS (A3 and A4 real restore)'
