#ifndef B2_RST_PSTREE_H
#define B2_RST_PSTREE_H

#include "restore.h"

#include <sys/types.h>

struct rst_item {
	pid_t pid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	unsigned int flags;
	size_t index;
	struct rst_item *parent;
	struct rst_item *children;
	struct rst_item *next_sibling;
};

struct rst_pstree {
	struct rst_item *items;
	size_t count;
	struct rst_item *root;
};

enum rst_item_flags {
	RST_ITEM_SESSION_LEADER = 1U << 0,
	RST_ITEM_PGRP_LEADER = 1U << 1,
};

enum b1_restore_status rst_read_pstree(const char *dir,
					struct rst_pstree *tree);
void rst_free_pstree(struct rst_pstree *tree);
enum b1_restore_status rst_validate_pstree(const struct rst_pstree *tree,
					    struct b1_restore_image *diag);
int rst_before_setsid(const struct rst_item *child);
const struct rst_item *rst_find_item(const struct rst_pstree *tree, pid_t pid);

#endif
