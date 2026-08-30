#!/bin/sh

set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR=$ROOT/tests/s0
ARTIFACT_ROOT=${S0_ARTIFACT_ROOT:-$ROOT/artifacts/s0}
GROUP=
ONLY_PROBE=
KEEP_WORKDIR=0
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)

usage()
{
	printf '%s\n' "usage: $0 [--group N] [--probe ID] [--keep-workdir] [--run-id ID]"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--group)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			GROUP=$2
			shift 2
			;;
		--probe)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			ONLY_PROBE=$2
			shift 2
			;;
		--keep-workdir)
			KEEP_WORKDIR=1
			shift
			;;
		--run-id)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			RUN_ID=$2
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			usage >&2
			exit 2
			;;
	esac
done

cleanup_transient()
{
	[ "$KEEP_WORKDIR" -eq 1 ] && return 0
	rm -f "$ROOT/tests/s0/.s0-probe"
	for evidence in \
		status report vmas fixture.out fixture-comm fixture-tgid \
		maps smaps maps-before maps-after smaps-before smaps-after smaps-delta \
		dmesg-before dmesg-after rmmod.error
	do
		rm -f "$ROOT/tests/s0/.s0-guest/$evidence"
	done
}
trap cleanup_transient EXIT

case "$RUN_ID" in
	''|.*|*/*|*..*) printf 'S0: invalid run id: %s\n' "$RUN_ID" >&2; exit 2 ;;
esac

RUN_DIR=$ARTIFACT_ROOT/$RUN_ID
if [ -e "$RUN_DIR" ]; then
	printf 'S0: run directory already exists: %s\n' "$RUN_DIR" >&2
	exit 2
fi
mkdir -p "$RUN_DIR" || exit 1

env_status=0
KDIR=${KDIR:-${HOME:-}/kernels/linux-5.10.29}
KDIR="$KDIR" "$SCRIPT_DIR/check-environment.sh" > "$RUN_DIR/environment.txt" 2>&1 || env_status=$?
if [ "$env_status" -ne 0 ]; then
	printf 'S0: environment check failed; evidence: %s\n' "$RUN_DIR/environment.txt" >&2
	printf '%s\n' "$RUN_DIR"
	exit "$env_status"
fi

source_status=0
LINUX_SRC=${LINUX_SRC:-/Users/yhome/workspace/source_code/linux-5.10.29} \
	"$SCRIPT_DIR/source-audit.sh" > "$RUN_DIR/source-audit.txt" 2>&1 || source_status=$?
if [ "$source_status" -ne 0 ]; then
	printf 'S0: source audit failed; evidence: %s\n' "$RUN_DIR/source-audit.txt" >&2
	printf '%s\n' "$RUN_DIR"
	exit "$source_status"
fi

cp "$SCRIPT_DIR/probes.tsv" "$RUN_DIR/manifest.tsv" || exit 1
awk -F '\t' -v group="$GROUP" -v wanted="$ONLY_PROBE" '
NR == 1 { print; next }
{
	if ((group == "" || $2 == group) && (wanted == "" || $1 == wanted))
		print
}
' "$SCRIPT_DIR/probes.tsv" > "$RUN_DIR/selected.tsv" || exit 1

selected_count=$(awk 'NR > 1 && NF { count++ } END { print count + 0 }' "$RUN_DIR/selected.tsv")
if [ "$selected_count" -eq 0 ]; then
	printf 'S0: no probes selected\n' >&2
	printf '%s\n' "$RUN_DIR"
	exit 2
fi

run_status=0
awk -F '\t' 'NR > 1 && NF { print $1 }' "$RUN_DIR/selected.tsv" |
while IFS= read -r probe; do
	printf 'S0: running %s\n' "$probe"
	if "$SCRIPT_DIR/run-one.sh" "$RUN_DIR" "$probe" < /dev/null; then
		:
	else
		run_status=1
	fi
done

# The loop above is deliberately allowed to finish every selected item. POSIX
# sh runs a piped while loop in a subshell, so derive failure from result files.
if awk -F= '
/^status=/ {
	if ($2 == "CRASH" || $2 == "CLEANUP-FAIL" ||
	    $2 == "WRONG-VALUE" || $2 == "ENV-MISMATCH") bad = 1
}
END { exit bad ? 1 : 0 }
' "$RUN_DIR"/*/result.txt 2>/dev/null; then
	:
else
	run_status=1
fi

summary_status=0
"$SCRIPT_DIR/summarize.sh" "$RUN_DIR" > "$RUN_DIR/summary.txt" 2>&1 || summary_status=$?
check_status=0
"$SCRIPT_DIR/check.sh" "$RUN_DIR" >> "$RUN_DIR/summary.txt" 2>&1 || check_status=$?

printf 'S0: run directory %s\n' "$RUN_DIR"
if [ "$KEEP_WORKDIR" -eq 0 ]; then
	# Evidence stays in artifacts/s0; transient guest files are cleaned by run-one.
	:
fi

if [ "$run_status" -ne 0 ] || [ "$summary_status" -ne 0 ] || [ "$check_status" -ne 0 ]; then
	exit 1
fi
exit 0
