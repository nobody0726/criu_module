#ifndef B2_RST_CLEANUP_H
#define B2_RST_CLEANUP_H

#include "rst_pstree.h"
#include "rst_shared.h"

int rst_cleanup_all(struct rst_shared *shared,
		    const struct rst_pstree *tree,
		    unsigned timeout_ms);
int rst_wait_tree_root(pid_t root_pid, unsigned timeout_ms);

#endif
