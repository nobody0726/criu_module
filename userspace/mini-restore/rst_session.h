#ifndef B2_RST_SESSION_H
#define B2_RST_SESSION_H

#include "rst_pstree.h"
#include "rst_shared.h"

int rst_restore_sid(const struct rst_item *item);
int rst_restore_pgid(struct rst_item *item, struct rst_shared *shared,
		     size_t leader_index, unsigned timeout_ms);
int rst_mark_ready(struct rst_shared *shared, size_t index);
int rst_wait_all_ready(struct rst_shared *shared, unsigned timeout_ms);
int rst_wait_commit_release(struct rst_shared *shared, unsigned timeout_ms);

#endif
