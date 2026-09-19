#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
carrier_c="$root_dir/userspace/mini-restore/carrier.c"
carrier_h="$root_dir/userspace/mini-restore/carrier.h"
files_c="$root_dir/userspace/mini-restore/files.c"
files_h="$root_dir/userspace/mini-restore/files.h"

make -C "$root_dir/userspace/mini-restore" clean all

test -s "$carrier_c"
test -s "$carrier_h"
test -s "$files_c"
test -s "$files_h"

grep -Fq 'struct clone_args' "$carrier_c"
grep -Fq 'SYS_clone3' "$carrier_c"
grep -Fq '.set_tid =' "$carrier_c"
grep -Fq '.set_tid_size = 1' "$carrier_c"
grep -Fq '.flags = 0' "$carrier_c"
grep -Fq '.exit_signal = SIGCHLD' "$carrier_c"
grep -Fq 'waitpid' "$carrier_c"
grep -Fq 'b1_carrier_manager_record' "$carrier_c"
grep -Fq 'b1_carrier_manager_cleanup' "$carrier_c"
grep -Fq 'EEXIST' "$carrier_c"
grep -Fq 'EPERM' "$carrier_c"
grep -Fq 'EINVAL' "$carrier_c"

if grep -Fq 'CLONE_VM' "$carrier_c" || grep -Fq 'CLONE_THREAD' "$carrier_c"; then
	echo "carrier must not share VM or thread group" >&2
	exit 1
fi

grep -Fq 'open(' "$files_c"
grep -Fq 'fstat(' "$files_c"
grep -Fq 'st_dev' "$files_c"
grep -Fq 'st_ino' "$files_c"
grep -Fq 'st_size' "$files_c"
grep -Fq 'lseek(' "$files_c"
grep -Fq 'FD_CLOEXEC' "$files_c"
grep -Fq 'b1_prepare_stdio' "$files_c"
grep -Fq 'b1_prepare_backing_file' "$files_c"

echo "B1_CARRIER_CONTRACT: PASS"
