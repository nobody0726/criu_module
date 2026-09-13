#!/bin/sh
set -eu

# One-test diagnostic probe. Keep the production allowlists unchanged while
# running pid00 through the same guest runner and shim.
probe_list=/tmp/a3-zdtm-probe-allowlist.txt
empty_list=/tmp/a3-zdtm-probe-empty.txt
echo "probe: PATH=$PATH"
command -v rm || true
ls -l /usr/bin/rm /bin/rm 2>&1 || true
/usr/bin/rm --version 2>&1 | head -2 || true
printf '%s\n' zdtm/static/pid00 > "$probe_list"
: > "$empty_list"
trap 'rm -f "$probe_list" "$empty_list"' EXIT HUP INT TERM

A_LIST="$probe_list" B_LIST="$empty_list" /bin/sh tests/ci-zdtm.sh
