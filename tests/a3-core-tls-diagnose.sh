#!/bin/sh
# Print the native CRIU AArch64 TLS value for the A3 minimal target.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CRIU=${CRIU:-}
TMP=$(mktemp -d /tmp/a3-core-tls.XXXXXX)
PID=0

cleanup()
{
	if [ "$PID" -gt 0 ] 2>/dev/null; then
		kill -9 "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

[ "$(id -u)" = 0 ] || { echo "A3_CORE_TLS: SKIP: root required"; exit 77; }
[ -n "$CRIU" ] || CRIU=$(command -v criu 2>/dev/null || true)
[ -n "$CRIU" ] || [ -x "$ROOT/criu/criu/criu" ] || {
	echo "A3_CORE_TLS: SKIP: criu unavailable"; exit 77;
}
[ -n "$CRIU" ] || CRIU=$ROOT/criu/criu/criu
[ -x "$ROOT/tests/progs/minimal" ] || {
	echo "A3_CORE_TLS: SKIP: minimal must be prebuilt"; exit 77;
}

: >"$TMP/stdin"
mkdir "$TMP/images"
(cd / && exec env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
	setsid "$ROOT/tests/progs/minimal" <"$TMP/stdin" >"$TMP/out" 2>"$TMP/err") &
PID=$!
for _ in $(seq 1 100); do
	grep -q '^pid=' "$TMP/out" 2>/dev/null && break
	sleep 0.05
done
grep -q '^pid=' "$TMP/out" || { cat "$TMP/err" >&2; exit 1; }

"$CRIU" dump -t "$PID" -D "$TMP/images" --leave-running --shell-job \
	-v4 --log-file="$TMP/dump.log" || {
	tail -80 "$TMP/dump.log" >&2
	exit 1
}

PYTHONPATH="$ROOT/criu/lib:$ROOT/criu/crit" \
	python3 -m crit decode -i "$TMP/images/core-$PID.img" --pretty >"$TMP/core.json"
python3 - "$TMP/core.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    core = json.load(source)["entries"][0]
aarch64 = core["ti_aarch64"]
gpregs = aarch64["gpregs"]
print("A3_CORE_TLS: tls=%s sp=%s pc=%s" %
      (aarch64["tls"], gpregs["sp"], gpregs["pc"]))
if int(aarch64["tls"]) == 0:
    raise SystemExit("A3_CORE_TLS: FAIL: native TLS is unexpectedly zero")
PY
