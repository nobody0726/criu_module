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

struct b1_aarch64_rt_sigframe {
	uint8_t siginfo[128];
	uint64_t flags;
	uint64_t link;
	uint64_t stack_sp;
	uint64_t stack_flags;
	uint64_t stack_size;
	uint64_t sigmask;
	struct b1_aarch64_sigcontext sc;
	struct b1_aarch64_fpsimd_context fpsimd;
	struct b1_aarch64_ctx end;
} __attribute__((aligned(16)));

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
