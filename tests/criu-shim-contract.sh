#!/bin/sh
# Contract for the A3 ZDTM shim. This test runs entirely in a temporary
# directory: the debugfs and CRIU/converter binaries are controlled fakes.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SHIM=$ROOT/userspace/criu-shim/criu-shim
TMP=$(mktemp -d "${TMPDIR:-/tmp}/criu-shim-contract.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

DEBUG_DIR=$TMP/debug
IMAGES=$TMP/images
mkdir -p "$DEBUG_DIR" "$IMAGES"
: >"$DEBUG_DIR/dump"

cat >"$TMP/converter" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$CONVERTER_ARGS"
touch "$3/converter-ran"
EOF
chmod +x "$TMP/converter"

cat >"$TMP/real-criu" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$REAL_CRIIU_ARGS"
EOF
chmod +x "$TMP/real-criu"

make -C "$ROOT/userspace" clean criu-shim >/dev/null

CRIU_DEBUG_DIR="$DEBUG_DIR" \
CRIU_CONVERTER="$TMP/converter" \
CONVERTER_ARGS="$TMP/converter.args" \
"$SHIM" dump --no-default-config --log-file dump.log \
	--images-dir "$IMAGES" --verbosity=4 --tree 4242 --leave-running

snapshot=$IMAGES/.criu-module-snapshot.bin
[ "$(cat "$DEBUG_DIR/dump")" = "4242 $snapshot" ]
[ "$(sed -n '1p' "$TMP/converter.args")" = "$snapshot" ]
[ "$(sed -n '2p' "$TMP/converter.args")" = "-D" ]
[ "$(sed -n '3p' "$TMP/converter.args")" = "$IMAGES" ]
[ -f "$IMAGES/converter-ran" ]

CRIU_REAL_BIN="$TMP/real-criu" \
REAL_CRIIU_ARGS="$TMP/real.args" \
"$SHIM" check --no-default-config --feature mem_track
[ "$(cat "$TMP/real.args")" = "check
--no-default-config
--feature
mem_track" ]

echo "CRIU_SHIM: PASS"
