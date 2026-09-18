#!/bin/sh
# The shared fixture suite checks magic, decoded protobuf tags, object IDs,
# raw payload framing and transactional output together.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bash "$ROOT/tests/a5-converter-fds.sh"
echo 'A5_IMAGES: PASS'
