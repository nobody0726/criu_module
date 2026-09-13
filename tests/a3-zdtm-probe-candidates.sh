#!/bin/sh
set -eu
probe_list=/tmp/a3-zdtm-probe-allowlist.txt
empty_list=/tmp/a3-zdtm-probe-empty.txt
printf '%s\n' \
  zdtm/static/pipe00 \
  zdtm/static/fd00 \
  zdtm/static/file_fown \
  zdtm/static/cwd00 \
  zdtm/static/auxv00 \
  zdtm/static/execfs00 \
  > "$probe_list"
: > "$empty_list"
trap 'rm -f "$probe_list" "$empty_list"' EXIT HUP INT TERM
A_LIST="$probe_list" B_LIST="$empty_list" /bin/sh tests/ci-zdtm.sh
