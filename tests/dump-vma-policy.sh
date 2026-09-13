#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
mm_c="$root_dir/kernel_module/checkpoint/dump_mm.c"
mm_h="$root_dir/kernel_module/checkpoint/dump_mm.h"
page_scan="$root_dir/kernel_module/checkpoint/page_scan.c"
makefile="$root_dir/kernel_module/Makefile"

for f in "$mm_c" "$mm_h" "$page_scan"; do test -s "$f"; done

# The dump ABI is semantic: raw vm_flags may be diagnostic only.
grep -q 'struct criu_vma_record' "$mm_h"
grep -q 'CRIU_VMA_DUMP_' "$mm_h"
grep -q 'CRIU_VMA_DUMP_PRIVATE_ANON' "$mm_h"
grep -q 'CRIU_VMA_DUMP_PRIVATE_FILE' "$mm_h"
grep -q 'CRIU_VMA_DUMP_VDSO' "$mm_h"
grep -q 'CRIU_VMA_DUMP_VVAR' "$mm_h"
grep -q 'CRIU_VMA_DUMP_SKIP_GUARD' "$mm_h"
grep -q 'VM_SHARED' "$mm_c"
grep -q 'VM_HUGETLB' "$mm_c"
grep -q 'VM_PFNMAP' "$mm_c"
grep -q 'VM_MIXEDMAP' "$mm_c"
grep -q 'VM_IO' "$mm_c"
grep -q 'EOPNOTSUPP' "$mm_c"
grep -q 'CRIU_SNAPSHOT_REC_MM' "$mm_h"
grep -q 'CRIU_SNAPSHOT_REC_VMA' "$mm_h"
grep -q 'checkpoint/dump_mm.o' "$makefile"

# Snapshot writes may sleep.  The VMA traversal and page scanner must finish
# collecting stable metadata before they emit records to the writer.
grep -q 'criu_snapshot_capture(task, &snapshot, false)' "$mm_c"
grep -q 'criu_snapshot_destroy(&snapshot)' "$mm_c"
! grep -q 'criu_walk_vmas(task, dump_one_vma' "$mm_c"
grep -q 'criu_snapshot_capture(task, &snapshot, false)' "$page_scan"
grep -q 'criu_snapshot_destroy(&snapshot)' "$page_scan"
! grep -q 'scan_vma(mm, vma, writer)' "$page_scan"

printf 'DUMP_VMA_POLICY: PASS\n'
