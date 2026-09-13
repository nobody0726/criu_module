#!/bin/sh
# Narrow the A3 restore investigation to CRIU itself.  The regular gate still
# decodes every generated image with crit; this wrapper bypasses that layer.
set -eu

export CRIT="$(dirname "$0")/crit-bypass.sh"
export RESTORE_TIMEOUT=${RESTORE_TIMEOUT:-45}
/bin/busybox ip link set lo up
exec "$(dirname "$0")/cross-restore.sh"
