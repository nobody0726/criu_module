#!/bin/sh
set -eu
cd /mnt/host/criu/test
echo "diag PATH=$PATH"
command -v make || true
command -v rm || true
type rm 2>&1 || true
ls -l /bin/rm /usr/bin/rm 2>&1 || true
make --no-print-directory -C zdtm/static -pn pid00.cleanout 2>&1 | grep -E '^(RM|MAKE|SHELL|PATH) ?[:+?]?=' | head -20 || true
make --no-print-directory -C zdtm/static -n pid00.cleanout 2>&1 | head -20
echo "direct make cleanout"
make --no-print-directory -C zdtm/static pid00.cleanout 2>&1 | head -30 || true
