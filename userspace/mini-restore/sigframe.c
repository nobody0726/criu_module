#include "sigframe.h"

#include <string.h>

int b1_sigframe_build(const struct b1_aarch64_thread_state *state,
		      struct b1_aarch64_rt_sigframe *frame)
{
	size_t i;

	if (!state || !frame)
		return -1;
	memset(frame, 0, sizeof(*frame));
	for (i = 0; i < 31; i++)
		frame->sc.regs[i] = state->regs[i];
	frame->sc.sp = state->sp;
	frame->sc.pc = state->pc;
	frame->sc.pstate = state->pstate;
	frame->sigmask = state->sigmask;
	frame->fpsimd.head.magic = B1_FPSIMD_MAGIC;
	frame->fpsimd.head.size = sizeof(frame->fpsimd);
	frame->fpsimd.fpsr = state->fpsr;
	frame->fpsimd.fpcr = state->fpcr;
	memcpy(frame->fpsimd.vregs, state->vregs, sizeof(frame->fpsimd.vregs));
	frame->end.magic = 0;
	frame->end.size = 0;
	return 0;
}
