#ifndef B2_RST_FORK_H
#define B2_RST_FORK_H

#include "rst_pstree.h"
#include "rst_shared.h"

struct b2_task_restore;

struct rst_tree_runtime;

struct rst_tree_context {
	struct rst_tree_runtime *runtime;
	size_t index;
};

struct rst_tree_runtime {
	struct rst_pstree *tree;
	struct rst_shared *shared;
	struct b2_task_restore *tasks;
	struct rst_tree_context *contexts;
	size_t task_count;
	unsigned timeout_ms;
};

typedef int (*rst_fork_task_fn)(const struct rst_item *item,
				const struct rst_item *parent,
				void *arg);

int rst_build_fork_order(const struct rst_pstree *tree,
			 const struct rst_item **order, size_t capacity,
			 size_t *first_pass_count);
int rst_fork_tree_two_pass(const struct rst_pstree *tree,
			   struct rst_shared *shared,
			   rst_fork_task_fn fn, void *arg);
int rst_create_children_and_session(struct rst_item *item,
				    struct rst_shared *shared,
				    struct b2_task_restore *task);
int rst_tree_restore_start(struct rst_tree_runtime *runtime,
			   unsigned root_carrier_flags);

#endif
