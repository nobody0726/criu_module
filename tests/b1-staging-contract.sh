#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
builder="$root_dir/tests/fixtures/b1-image-builder.py"
tmp="${TMPDIR:-/tmp}/b1-staging-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

make -C "$root_dir/userspace/mini-restore" clean all

cat > "$tmp/harness.c" <<'C'
#include <stdio.h>
#include "image_reader.h"
#include "staging.h"
#include "validator.h"

int main(int argc, char **argv)
{
	struct b1_restore_image image;
	struct b1_staging_plan plan;
	enum b1_restore_status st;

	if (argc != 2)
		return 2;
	b1_restore_image_init(&image);
	b1_staging_plan_init(&plan);
	st = b1_read_images(argv[1], &image);
	if (st == B1_RESTORE_OK)
		st = b1_validate_supported(&image);
	if (st == B1_RESTORE_OK)
		st = b1_stage_image(&image, argv[1], &plan);
	if (st != B1_RESTORE_OK) {
		printf("%s\n", image.diagnostic);
		b1_staging_plan_free(&plan);
		b1_restore_image_free(&image);
		return 1;
	}
	if (plan.vma_count != image.vma_count || !plan.restore_vmas || !plan.arena)
		return 3;
	if (plan.restore_vmas[0].staging_start == plan.restore_vmas[0].target_start)
		return 4;
	if (plan.restore_vmas[0].length != image.vmas[0].length)
		return 5;
	printf("staged=%zu arena=%zu\n", plan.vma_count, plan.arena_len);
	b1_staging_plan_free(&plan);
	b1_restore_image_free(&image);
	return 0;
}
C

cc -std=c11 -Wall -Wextra -Werror \
	-I"$root_dir/userspace/mini-restore" -I"$root_dir/include" \
	"$tmp/harness.c" "$root_dir/userspace/mini-restore/libmini_restore.a" \
	-o "$tmp/harness"

python3 "$builder" valid "$tmp/valid"
"$tmp/harness" "$tmp/valid" > "$tmp/valid.out"
grep -Fq 'staged=2' "$tmp/valid.out"

staging_c="$root_dir/userspace/mini-restore/staging.c"
grep -Fq 'mmap(NULL' "$staging_c"
grep -Fq 'MAP_PRIVATE | MAP_ANONYMOUS' "$staging_c"
grep -Fq 'pread(' "$staging_c"
grep -Fq 'mprotect(' "$staging_c"
grep -Fq 'CRIU_RESTORE_VMA_ANON_PRIVATE' "$staging_c"
grep -Fq 'CRIU_RESTORE_VMA_FILE_PRIVATE' "$staging_c"
grep -Fq 'CRIU_RESTORE_VMA_F_GROWSDOWN' "$staging_c"
grep -Fq 'B1_RESTORE_PAGE_SIZE' "$staging_c"
grep -Fq 'page_offset = page_index * B1_RESTORE_PAGE_SIZE' "$staging_c"
grep -Fq 'b1_stage_page_runs' "$staging_c"
grep -Fq 'b1_ranges_overlap' "$staging_c"

echo "B1_STAGING_CONTRACT: PASS"
