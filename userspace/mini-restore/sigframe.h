#ifndef B1_SIGFRAME_H
#define B1_SIGFRAME_H

#include <stddef.h>
#include <stdint.h>

#define B1_FPSIMD_MAGIC 0x46508001U
#define B1_AARCH64_RESERVED_SIZE 4096U

struct b1_aarch64_ctx {
	uint32_t magic;
	uint32_t size;
};

struct b1_aarch64_fpsimd_context {
	struct b1_aarch64_ctx head;
	uint32_t fpsr;
	uint32_t fpcr;
	uint8_t vregs[32][16];
};

struct b1_aarch64_sigcontext {
	uint64_t fault_address;
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint8_t reserved[B1_AARCH64_RESERVED_SIZE] __attribute__((aligned(16)));
};

/* Linux arm64 ucontext layout from arch/arm64/include/uapi/asm/ucontext.h. */
struct b1_aarch64_ucontext {
	uint64_t flags;
	uint64_t link;
	uint64_t stack_sp;
	uint64_t stack_flags;
	uint64_t stack_size;
	uint64_t sigmask;
	uint8_t sigmask_padding[120];
	uint64_t mcontext_align;
	struct b1_aarch64_sigcontext mcontext;
};

/* Compatibility view used by the focused contract tests.  Its members are
 * overlaid on the exact ucontext layout below; fpsimd/end therefore start at
 * the beginning of sigcontext.__reserved, where Linux expects them. */
struct b1_aarch64_sigcontext_header {
	uint64_t fault_address;
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
};

struct b1_aarch64_rt_sigframe {
	uint8_t siginfo[128];
	union {
		struct b1_aarch64_ucontext uc;
		struct {
			uint64_t flags;
			uint64_t link;
			uint64_t stack_sp;
			uint64_t stack_flags;
			uint64_t stack_size;
			union {
				uint8_t sigmask_bytes[128];
				uint64_t sigmask;
			};
			uint64_t mcontext_align;
			struct b1_aarch64_sigcontext_header sc;
			uint64_t reserved_align;
			uint8_t reserved[4096];
		};
	};
	uint64_t fp;
	uint64_t lr;
} __attribute__((aligned(16)));

/* The aux records live at the beginning of sigcontext.__reserved. */
static inline struct b1_aarch64_fpsimd_context *
b1_sigframe_fpsimd(struct b1_aarch64_rt_sigframe *frame)
{
	return (struct b1_aarch64_fpsimd_context *)frame->uc.mcontext.reserved;
}

static inline const struct b1_aarch64_fpsimd_context *
b1_sigframe_fpsimd_const(const struct b1_aarch64_rt_sigframe *frame)
{
	return (const struct b1_aarch64_fpsimd_context *)frame->uc.mcontext.reserved;
}

static inline struct b1_aarch64_ctx *
b1_sigframe_end(struct b1_aarch64_rt_sigframe *frame)
{
	return (struct b1_aarch64_ctx *)(frame->uc.mcontext.reserved +
					 sizeof(struct b1_aarch64_fpsimd_context));
}

struct b1_aarch64_thread_state {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t sigmask;
	uint64_t tls;
	uint8_t vregs[32][16];
	uint32_t fpsr;
	uint32_t fpcr;
};

int b1_sigframe_build(const struct b1_aarch64_thread_state *state,
		      struct b1_aarch64_rt_sigframe *frame);

#endif
