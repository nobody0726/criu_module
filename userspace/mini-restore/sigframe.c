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
		frame->uc.mcontext.regs[i] = state->regs[i];
	frame->uc.mcontext.sp = state->sp;
	frame->uc.mcontext.pc = state->pc;
	frame->uc.mcontext.pstate = state->pstate;
	frame->uc.sigmask = state->sigmask;
	{
		struct b1_aarch64_fpsimd_context *fpsimd = b1_sigframe_fpsimd(frame);
		struct b1_aarch64_ctx *end = b1_sigframe_end(frame);

		fpsimd->head.magic = B1_FPSIMD_MAGIC;
		fpsimd->head.size = sizeof(*fpsimd);
		fpsimd->fpsr = state->fpsr;
		fpsimd->fpcr = state->fpcr;
		memcpy(fpsimd->vregs, state->vregs, sizeof(fpsimd->vregs));
		end->magic = 0;
		end->size = 0;
	}
	return 0;
}
