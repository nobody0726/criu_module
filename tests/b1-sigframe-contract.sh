#!/bin/sh
set -eu

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
tmp="${TMPDIR:-/tmp}/b1-sigframe-contract.$$"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

make -C "$root_dir/userspace/mini-restore" clean all

cat > "$tmp/harness.c" <<'C'
#include <stdint.h>
#include <stdio.h>
#include "sigframe.h"

int main(void)
{
	struct b1_aarch64_thread_state state = {0};
	struct b1_aarch64_rt_sigframe frame;
	size_t i;

	for (i = 0; i < 31; i++)
		state.regs[i] = 0x1000 + i;
	state.sp = 0x70000000;
	state.pc = 0x400010;
	state.pstate = 0x60000000;
	state.sigmask = 0x55;
	state.tls = 0x12345000;
	state.fpsr = 1;
	state.fpcr = 2;
	for (i = 0; i < 32; i++)
		state.vregs[i][0] = (uint8_t)i;

	if (b1_sigframe_build(&state, &frame))
		return 1;
	if (((uintptr_t)&frame % 16) != 0)
		return 2;
	if (frame.sc.regs[30] != 0x101e || frame.sc.sp != state.sp ||
	    frame.sc.pc != state.pc || frame.sc.pstate != state.pstate)
		return 3;
	if (frame.sigmask != state.sigmask)
		return 4;
	if (frame.fpsimd.head.magic != B1_FPSIMD_MAGIC ||
	    frame.fpsimd.head.size != sizeof(frame.fpsimd))
		return 5;
	if (frame.fpsimd.vregs[31][0] != 31 || frame.fpsimd.fpsr != 1 ||
	    frame.fpsimd.fpcr != 2)
		return 6;
	if (state.tls != 0x12345000)
		return 7;
	printf("sigframe=%zu fpsimd=%zu\n", sizeof(frame), sizeof(frame.fpsimd));
	return 0;
}
C

cc -std=c11 -Wall -Wextra -Werror \
	-I"$root_dir/userspace/mini-restore" -I"$root_dir/include" \
	"$tmp/harness.c" "$root_dir/userspace/mini-restore/libmini_restore.a" \
	-o "$tmp/harness"
"$tmp/harness" > "$tmp/harness.out"
grep -Fq 'sigframe=' "$tmp/harness.out"

sigframe_h="$root_dir/userspace/mini-restore/sigframe.h"
bootstrap_s="$root_dir/userspace/mini-restore/bootstrap.S"

grep -Fq 'regs[31]' "$sigframe_h"
grep -Fq 'vregs[32][16]' "$sigframe_h"
grep -Fq 'B1_FPSIMD_MAGIC 0x46508001U' "$sigframe_h"
grep -Fq 'tls' "$sigframe_h"

grep -Fq 'b1_restore_bootstrap_entry' "$bootstrap_s"
grep -Fq 'svc #0' "$bootstrap_s"
grep -Fq 'msr tpidr_el0' "$bootstrap_s"
grep -Fq '__NR_rt_sigreturn' "$bootstrap_s"
grep -Fq '__NR_ioctl' "$bootstrap_s"
grep -Fq 'mov sp' "$bootstrap_s"
if grep -Eq '\\bbl\\b|printf|malloc|__libc|\\bret\\b' "$bootstrap_s"; then
	echo "bootstrap must not call libc or return to C" >&2
	exit 1
fi

echo "B1_SIGFRAME_CONTRACT: PASS"
