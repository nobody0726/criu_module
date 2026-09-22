#ifndef B1_BOOTSTRAP_H
#define B1_BOOTSTRAP_H

#include <stdint.h>

struct b1_bootstrap_args {
	uint64_t restore_fd;
	uint64_t commit_user_ptr;
	uint64_t sigframe_final_sp;
	uint64_t tls;
	uint64_t bootstrap_sp;
	uint64_t target_sp;
	uint64_t target_pc;
	uint64_t target_pc_word;
	uint64_t target_stop;
};

void b1_restore_bootstrap_entry(const struct b1_bootstrap_args *args);

#endif
