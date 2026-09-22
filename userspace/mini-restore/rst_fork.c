#include "rst_fork.h"

#include "rst_session.h"
#include "task_restore.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int tree_carrier_entry(void *arg);

static int item_requires_first_pass(const struct rst_item *item)
{
	const struct rst_item *child;

	if (item->parent && rst_before_setsid(item))
		return 1;
	for (child = item->children; child; child = child->next_sibling)
		if (item_requires_first_pass(child))
			return 1;
	return 0;
}

static size_t append_preorder(const struct rst_item *item,
			      const struct rst_item **order, size_t at,
			      int first_pass)
{
	const struct rst_item *child;
	int include = item_requires_first_pass(item) == first_pass;

	if (include)
		order[at++] = item;
	for (child = item->children; child; child = child->next_sibling)
		at = append_preorder(child, order, at, first_pass);
	return at;
}

int rst_build_fork_order(const struct rst_pstree *tree,
			 const struct rst_item **order, size_t capacity,
			 size_t *first_pass_count)
{
	size_t first_count;
	size_t total;

	if (!tree || !tree->root || !order || !first_pass_count ||
	    capacity < tree->count)
		return -EINVAL;
	first_count = append_preorder(tree->root, order, 0, 1);
	total = append_preorder(tree->root, order, first_count, 0);
	if (total != tree->count)
		return -EINVAL;
	*first_pass_count = first_count;
	return 0;
}

int rst_fork_tree_two_pass(const struct rst_pstree *tree,
			   struct rst_shared *shared,
			   rst_fork_task_fn fn, void *arg)
{
	const struct rst_item **order;
	size_t first_count;
	size_t i;
	int rc;

	if (!tree || !shared || !fn)
		return -EINVAL;
	order = calloc(tree->count, sizeof(*order));
	if (!order)
		return -ENOMEM;
	rc = rst_build_fork_order(tree, order, tree->count, &first_count);
	if (rc)
		goto out;
	for (i = 0; i < tree->count; i++) {
		const struct rst_item *item = order[i];

		rc = fn(item, item->parent, arg);
		if (rc)
			goto out;
		if (i + 1U == first_count)
			(void)shared;
	}
out:
	free(order);
	return rc;
}

int rst_create_children_and_session(struct rst_item *item,
				    struct rst_shared *shared,
				    struct b2_task_restore *task)
{
	(void)shared;
	(void)task;
	if (!item)
		return -EINVAL;
	return rst_restore_sid(item);
}

static int tree_spawn_child(struct rst_tree_context *context,
			    const struct rst_item *child)
{
	struct rst_tree_runtime *runtime = context->runtime;
	struct b2_task_restore *task = &runtime->tasks[child->index];
	pid_t pid;
	enum b1_restore_status st;

	st = b1_task_restore_spawn(task, B1_CARRIER_F_KEEP_PARENT_TLS,
				   tree_carrier_entry,
				   &runtime->contexts[child->index]);
	if (st != B1_RESTORE_OK) {
		rst_shared_abort(runtime->shared, -st);
		return -1;
	}
	if (!task->cleanup.carriers.count)
		return -1;
	pid = task->cleanup.carriers.pids[task->cleanup.carriers.count - 1U];
	if (rst_shared_register_pid(runtime->shared, child->index, pid) != 0) {
		rst_shared_abort(runtime->shared, -EIO);
		return -1;
	}
	return 0;
}

static int tree_spawn_children(struct rst_tree_context *context,
			       const struct rst_item *item, int first_pass)
{
	const struct rst_item *child;

	for (child = item->children; child; child = child->next_sibling) {
		if (!!rst_before_setsid(child) != !!first_pass)
			continue;
		if (tree_spawn_child(context, child))
			return -1;
	}
	return 0;
}

static int tree_carrier_entry(void *arg)
{
	struct rst_tree_context *context = arg;
	struct rst_tree_runtime *runtime;
	struct rst_item *item;
	struct rst_item *leader;
	size_t leader_index;
	int rc;

	if (!context || !context->runtime ||
	    context->index >= context->runtime->task_count)
		return 127;
	runtime = context->runtime;
	item = &runtime->tree->items[context->index];

	/*
	 * Children that must inherit the parent's pre-setsid session are
	 * created before the current task changes session, matching CRIU's
	 * restore_before_setsid() ordering.
	 */
	if (tree_spawn_children(context, item, 1))
		goto fail;
	if (rst_restore_sid(item))
		goto fail;
	if (tree_spawn_children(context, item, 0))
		goto fail;

	leader = (struct rst_item *)rst_find_item(runtime->tree, item->pgid);
	if (!leader)
		goto fail;
	leader_index = leader->index;
	rc = rst_restore_pgid(item, runtime->shared, leader_index,
			      runtime->timeout_ms);
	if (rc)
		goto fail;
	if (rst_mark_ready(runtime->shared, item->index))
		goto fail;
	if (rst_wait_all_ready(runtime->shared, runtime->timeout_ms))
		goto fail;
	if (item == runtime->tree->root) {
		if (rst_shared_release_commit(runtime->shared))
			goto fail;
	} else if (rst_wait_commit_release(runtime->shared,
					   runtime->timeout_ms)) {
		goto fail;
	}
	rc = b1_task_restore_bootstrap_now(
		&runtime->tasks[context->index]);
	if (rc)
		goto fail;
	return 127;

fail:
	rst_shared_abort(runtime->shared, -EIO);
	return 127;
}

int rst_tree_restore_start(struct rst_tree_runtime *runtime,
			   unsigned root_carrier_flags)
{
	struct rst_item *root;
	struct b2_task_restore *root_task;
	pid_t root_pid;
	enum b1_restore_status st;

	if (!runtime || !runtime->tree || !runtime->shared ||
	    !runtime->tasks || !runtime->contexts ||
	    !runtime->tree->root || !runtime->task_count)
		return -EINVAL;
	root = runtime->tree->root;
	root_task = &runtime->tasks[root->index];
	runtime->contexts[root->index].runtime = runtime;
	runtime->contexts[root->index].index = root->index;
	st = b1_task_restore_spawn(
		root_task, root_carrier_flags | B1_CARRIER_F_KEEP_PARENT_TLS,
				   tree_carrier_entry,
				   &runtime->contexts[root->index]);
	if (st != B1_RESTORE_OK)
		return -st;
	if (!root_task->cleanup.carriers.count)
		return -EIO;
	root_pid = root_task->cleanup.carriers.pids[
		root_task->cleanup.carriers.count - 1U];
	return rst_shared_register_pid(runtime->shared, root->index, root_pid);
}
