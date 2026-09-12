#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
scan_c="$root_dir/kernel_module/checkpoint/page_scan.c"
scan_h="$root_dir/kernel_module/checkpoint/page_scan.h"
mm_c="$root_dir/kernel_module/checkpoint/dump_mm.c"
makefile="$root_dir/kernel_module/Makefile"

for f in "$scan_c" "$scan_h"; do test -s "$f"; done
grep -q 'FOLL_NOFAULT' "$scan_c"
grep -q 'get_user_pages_remote' "$scan_c"
grep -q 'is_zero_pfn' "$scan_c"
grep -q 'PageAnon' "$scan_c"
grep -q 'VM_DONTDUMP' "$scan_c"
grep -q '\[vvar\]' "$scan_c"
grep -q '\[vdso\]' "$scan_c"
grep -q 'EOPNOTSUPP' "$scan_c"
grep -q 'CRIU_SNAPSHOT_REC_PAGE_RUN' "$scan_c"
grep -q 'criu_dump_pages' "$mm_c"
grep -q 'checkpoint/page_scan.o' "$makefile"
printf 'PAGE_POLICY: PASS\n'
