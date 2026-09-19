#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
builder="$root_dir/tests/fixtures/b1-image-builder.py"
tmp="${TMPDIR:-/tmp}/b1-cleanup-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

make -C "$root_dir/userspace/mini-restore" clean all

bin="$root_dir/userspace/mini-restore/mini-restore"
test -x "$bin"

python3 "$builder" wrong-arch "$tmp/wrong-arch"
if "$bin" --images "$tmp/wrong-arch" --dry-run > "$tmp/wrong.out" 2>&1; then
	echo "wrong arch unexpectedly passed" >&2
	exit 1
fi
grep -Fq 'UNSUPPORTED' "$tmp/wrong.out"

python3 "$builder" valid "$tmp/valid"
"$bin" --images "$tmp/valid" --dry-run > "$tmp/valid.out" 2>&1
grep -Fq 'B1_RESTORE: DRY_RUN_OK' "$tmp/valid.out"

cleanup_c="$root_dir/userspace/mini-restore/cleanup.c"
main_c="$root_dir/userspace/mini-restore/main.c"

grep -Fq 'b1_cleanup_record_pid' "$cleanup_c"
grep -Fq 'b1_cleanup_run' "$cleanup_c"
grep -Fq 'kill(' "$cleanup_c"
grep -Fq 'waitpid' "$cleanup_c"
grep -Fq 'b1_carrier_manager_cleanup' "$cleanup_c"
grep -Fq 'b1_staging_plan_free' "$cleanup_c"

for token in \
	'b1_read_images' \
	'b1_validate_supported' \
	'open("/dev/criu_restore"' \
	'CRIU_RESTORE_VALIDATE_V1' \
	'b1_create_exact_pid_carrier' \
	'b1_stage_image' \
	'b1_sigframe_build' \
	'b1_cleanup_run'
do
	grep -Fq "$token" "$main_c"
done

if grep -Fq 'kill(0' "$cleanup_c" || grep -Fq 'killpg' "$cleanup_c"; then
	echo "cleanup must use recorded PIDs, not process-group cleanup" >&2
	exit 1
fi

echo "B1_CLEANUP_CONTRACT: PASS"
