#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
builder="$root_dir/tests/fixtures/b1-image-builder.py"
tmp="${TMPDIR:-/tmp}/b1-image-reader-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

make -C "$root_dir/userspace/mini-restore" clean all

cat > "$tmp/harness.c" <<'C'
#include <stdio.h>
#include "image_reader.h"
#include "validator.h"

int main(int argc, char **argv)
{
	struct b1_restore_image image;
	enum b1_restore_status st;

	if (argc != 2)
		return 2;
	b1_restore_image_init(&image);
	st = b1_read_images(argv[1], &image);
	if (st == B1_RESTORE_OK)
		st = b1_validate_supported(&image);
	printf("%s:%s\n", b1_restore_status_name(st), image.diagnostic);
	b1_restore_image_free(&image);
	return st == B1_RESTORE_OK ? 0 : 1;
}
C

cc -std=c11 -Wall -Wextra -Werror \
	-I"$root_dir/userspace/mini-restore" -I"$root_dir/include" \
	"$tmp/harness.c" "$root_dir/userspace/mini-restore/libmini_restore.a" \
	-o "$tmp/harness"

python3 "$builder" valid "$tmp/valid"
"$tmp/harness" "$tmp/valid" > "$tmp/valid.out"
grep -Fq 'OK:' "$tmp/valid.out"

for case in missing-mm unstable-file; do
	python3 "$builder" "$case" "$tmp/$case"
	if "$tmp/harness" "$tmp/$case" > "$tmp/$case.out"; then
		echo "$case unexpectedly passed" >&2
		exit 1
	fi
	grep -Fq 'IO:' "$tmp/$case.out"
done

for case in wrong-arch threads children shared-mm namespaces shared-mapping \
	dirty-file-private vdso-reloc pac sve gcs; do
	python3 "$builder" "$case" "$tmp/$case"
	if "$tmp/harness" "$tmp/$case" > "$tmp/$case.out"; then
		echo "$case unexpectedly passed" >&2
		exit 1
	fi
	grep -Fq 'UNSUPPORTED:' "$tmp/$case.out"
done

for case in overlap unaligned page-overflow bad-field short-file; do
	python3 "$builder" "$case" "$tmp/$case"
	if "$tmp/harness" "$tmp/$case" > "$tmp/$case.out"; then
		echo "$case unexpectedly passed" >&2
		exit 1
	fi
	grep -Fq 'FORMAT:' "$tmp/$case.out"
done

echo "B1_IMAGE_READER_CONTRACT: PASS"
