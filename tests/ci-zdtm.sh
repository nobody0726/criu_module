#!/bin/sh
# ci-zdtm.sh -- run the allowlisted ZDTM subsets, in-guest, as root.
# Invoked by scripts/run-qemu.sh --ci --script tests/ci-zdtm.sh.
#
# Contract with .github/workflows/qemu-test.yml:
#   the LAST line on success is exactly "ZDTM_RESULT: PASS".
#
# Two allowlists, two directions, one test suite:
#   ci/zdtm-allowlist.txt          -> criu-shim   (our dump  + real restore)
#   ci/zdtm-restore-allowlist.txt  -> restore-shim (real dump + our restore)
#
# An allowlist containing only comments is a legitimate state (pre-A3 / pre-B1).
# This script, not the workflow, is the authority on that: the workflow's file
# test cannot tell a comment-only file from a populated one.
set -eu

# ZDTM changes uid/gid and creates setuid test files.  A 9p project mount is
# intentionally unsuitable for that, so run the suite from a guest-local
# copy while retaining the host mount only as the input artifact source.
HOST_PROJECT_ROOT=$(pwd)
LOCAL_ROOT=${ZDTM_LOCAL_ROOT:-/tmp/criu-zdtm-run}
rm -rf "$LOCAL_ROOT"
mkdir -p "$LOCAL_ROOT"
cp -a "$HOST_PROJECT_ROOT/criu" "$LOCAL_ROOT/criu"
cp -a "$HOST_PROJECT_ROOT/userspace" "$LOCAL_ROOT/userspace"
cp -a "$HOST_PROJECT_ROOT/kernel_module" "$LOCAL_ROOT/kernel_module"
cp -a "$HOST_PROJECT_ROOT/ci" "$LOCAL_ROOT/ci"

CRIU_SRC=${CRIU_SRC:-$LOCAL_ROOT/criu}
PROJECT_ROOT=$LOCAL_ROOT
A_LIST=${A_LIST:-$LOCAL_ROOT/ci/zdtm-allowlist.txt}
B_LIST=${B_LIST:-$LOCAL_ROOT/ci/zdtm-restore-allowlist.txt}
A_SHIM=${A_SHIM:-$LOCAL_ROOT/userspace/criu-shim/criu-shim}
B_SHIM=${B_SHIM:-$LOCAL_ROOT/userspace/mini-restore/restore-shim}
MODULE=${MODULE:-$LOCAL_ROOT/kernel_module/criu_kernel.ko}
export CRIU_CONVERTER=${CRIU_CONVERTER:-$PROJECT_ROOT/userspace/criu-module-convert/criu-module-convert}
export CRIU_REAL_BIN=${CRIU_REAL_BIN:-$PROJECT_ROOT/criu/criu/criu}
# The minimal guest mounts Lima's /usr but not /etc.  On Ubuntu, /usr/bin/cc
# points through /etc/alternatives, while gcc is a self-contained sibling link.
export CC=${CC:-/usr/bin/gcc}
# The initramfs provides GNU rm at /usr/bin.  ZDTM's Makefiles use an
# immediately-assigned RM variable, so ask make to honor this explicit path.
export RM=${RM:-/usr/bin/rm -f --one-file-system}
export MAKEFLAGS="${MAKEFLAGS:-} -e"

fail() {
	echo "ZDTM: $*" >&2
	echo "ZDTM_RESULT: FAIL"
	exit 1
}

[ "$(id -u)" = "0" ] || fail "must run as root inside the guest (zdtm needs it)"
[ -d "$CRIU_SRC/test" ] || fail "no zdtm at $CRIU_SRC/test (set CRIU_SRC)"

# Strip comments and blank lines. This is the same expression the workflow uses
# for its progress count, so the two never disagree about what "empty" means.
entries() {
	[ -f "$1" ] || return 0
	grep -vE '^[[:space:]]*(#|$)' "$1" || true
}

TOTAL_RUN=0
FAILED=""

# ---------------------------------------------------------------------------
# run_track <label> <allowlist> <shim>
#
# Runs each test individually rather than passing the whole list to zdtm.py at
# once. One zdtm.py invocation per test costs a few seconds of startup but buys
# per-test attribution and means one hanging test cannot take the batch's
# results with it -- worth it on an allowlist this small.
# ---------------------------------------------------------------------------
run_track() {
	label="$1"
	list="$2"
	shim="$3"

	n=$(entries "$list" | wc -l | tr -d ' ')
	if [ "$n" = "0" ]; then
		echo "ZDTM: [$label] allowlist has no active entries; nothing to run"
		return 0
	fi

	[ -x "$shim" ] || fail "[$label] allowlist has $n entries but $shim is missing or not executable"

	echo "ZDTM: [$label] $n test(s) via $shim"
	case "$shim" in
	/*) shim_path="$shim" ;;
	*) shim_path="$PROJECT_ROOT/$shim" ;;
	esac

	entries "$list" | while read -r t; do
		echo "ZDTM: [$label] --- $t"
		# --criu-bin is what makes the shim the thing under test: zdtm.py
		# calls it wherever it would call criu, so the shim decides which
		# half is ours and which half is the oracle.
		if (cd "$CRIU_SRC/test" && \
		    timeout 300 ./zdtm.py run -t "$t" --criu-bin "$shim_path" \
			--ignore-taint -f h); then
			echo "ZDTM: [$label] PASS $t"
		else
			rc=$?
			[ "$rc" = "124" ] && echo "ZDTM: [$label] TIMEOUT $t" >&2
			echo "ZDTM: [$label] FAIL $t (rc=$rc)" >&2
			# Written to a file because this runs in a `while read`
			# subshell -- a variable assignment here would not survive.
			echo "$label:$t" >> /tmp/zdtm-failed.txt
			# Its log is the only thing that explains why.
			sed -n '1,80p' "$CRIU_SRC/test/$t.out" 2>/dev/null || true
		fi
	done

	TOTAL_RUN=$((TOTAL_RUN + n))
}

rm -f /tmp/zdtm-failed.txt

if [ "$(entries "$A_LIST" | wc -l | tr -d ' ')" != "0" ]; then
	[ -f "$MODULE" ] || fail "[dump] module is missing: $MODULE"
	insmod "$MODULE" || fail "[dump] cannot load module"
	trap 'rmmod criu_kernel 2>/dev/null || true' EXIT HUP INT TERM
fi

run_track "dump" "$A_LIST" "$A_SHIM"
run_track "restore" "$B_LIST" "$B_SHIM"

rmmod criu_kernel 2>/dev/null || true
trap - EXIT HUP INT TERM

if [ -s /tmp/zdtm-failed.txt ]; then
	echo "ZDTM: failures:" >&2
	cat /tmp/zdtm-failed.txt >&2
	FAILED=$(wc -l < /tmp/zdtm-failed.txt | tr -d ' ')
	fail "$FAILED allowlisted test(s) failed"
fi

if [ "$TOTAL_RUN" = "0" ]; then
	# Not a failure: both allowlists are comment-only, which is exactly the
	# state before A3 and B1 land. Saying so explicitly keeps a green CI from
	# being mistaken for "the tests passed".
	echo "ZDTM: both allowlists empty -- no coverage yet, nothing claimed"
fi

echo "ZDTM: $TOTAL_RUN test(s) run, 0 failures"
echo "ZDTM_RESULT: PASS"
