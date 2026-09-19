#!/bin/sh
# Contract for the B1 VALIDATE transaction.  The real ioctl is introduced by
# the kernel patch; this test locks the negative cases and the TOCTOU boundary
# that later guest tests exercise through /dev/criu_restore.
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
patch4="$root_dir/patches/linux-5.10.29/0004-criu-restore-mm-helper.patch"
builder="$root_dir/tests/fixtures/b1-restore-plan-builder.py"
tmp="${TMPDIR:-/tmp}/b1-validate-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

python3 "$builder" valid > "$tmp/valid.json"
python3 - "$tmp/valid.json" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["errors"] == [], data["errors"]
PY

for case in \
	bad-version bad-size zero-target-pid zero-vmas too-many-vmas \
	unaligned duplicate-target overflow bad-kind bad-plan-flags bad-vma-flags \
	bad-bootstrap-pc bad-bootstrap-sp bad-sigframe-staging bad-sigframe-final
do
	python3 "$builder" "$case" > "$tmp/$case.json"
	python3 - "$tmp/$case.json" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["errors"], data
PY
done

grep -Fq 'copy_from_user(&plan' "$patch4"
grep -Fq 'copy_from_user(vmas' "$patch4"
grep -Fq 'tx->plan = plan' "$patch4"
grep -Fq 'tx->vmas = vmas' "$patch4"
grep -Fq 'tx->state != RESTORE_TX_OPEN' "$patch4"
grep -Fq 'current->mm' "$patch4"
grep -Fq 'criu_restore_validate_bootstrap' "$patch4"
grep -Fq 'criu_restore_validate_sigframe' "$patch4"
grep -Fq 'vmas_user = (void __user *)(unsigned long)plan.vmas_user_ptr' "$patch4"
grep -Fq 'vmas = NULL' "$patch4"

if grep -Fq 'tx->plan.vmas_user_ptr' "$patch4"; then
	echo "COMMIT/transaction code must not use the original user VMA pointer" >&2
	exit 1
fi

echo "B1_VALIDATE_CONTRACT: PASS"
