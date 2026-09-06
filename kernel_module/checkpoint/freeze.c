/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

#include "criu_kernel.h"

enum criu_freeze_state {
	CRIU_FREEZE_IDLE,
	CRIU_FREEZE_FREEZING,
	CRIU_FREEZE_FROZEN_SETTLED,
	CRIU_FREEZE_THAWING,
	CRIU_FREEZE_ROLLBACK,
};

struct criu_freeze_ctx {
	struct task_struct *target;
	u64 target_generation;
	enum criu_freeze_state state;
	bool include_children;
};

static DEFINE_MUTEX(criu_freeze_lock);
static struct criu_freeze_ctx *criu_freeze_current;
static enum criu_freeze_state criu_freeze_current_state = CRIU_FREEZE_IDLE;

/*
 * This lockless query is deliberately small: target.c calls it while holding
 * the target mutex, while criu_freeze() holds the freeze mutex before taking
 * the target reference. Publishing FREEZING first prevents target replacement
 * during the ownership hand-off without creating a lock-order cycle.
 */
bool criu_freeze_context_active(void)
{
	return READ_ONCE(criu_freeze_current_state) != CRIU_FREEZE_IDLE;
}

int criu_freeze(pid_t vpid, bool include_children,
		struct criu_freeze_ctx **ctx)
{
	struct criu_freeze_ctx *new_ctx;
	struct task_struct *target;
	u64 generation;

	if (!ctx || vpid <= 0)
		return -EINVAL;
	if (ctx)
		*ctx = NULL;
	if (include_children)
		return -EOPNOTSUPP;

	mutex_lock(&criu_freeze_lock);
	if (criu_freeze_current) {
		mutex_unlock(&criu_freeze_lock);
		return -EBUSY;
	}

	new_ctx = kzalloc(sizeof(*new_ctx), GFP_KERNEL);
	if (!new_ctx) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOMEM;
	}

	/*
	 * Task 2 establishes ownership and generation binding. Actual freezer
	 * movement and settled detection are added by later A2 tasks.
	 */
	criu_freeze_current = new_ctx;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_FREEZING);
	target = criu_target_get(&generation);
	if (!target || task_pid_vnr(target) != vpid) {
		if (target)
			put_task_struct(target);
		criu_freeze_current = NULL;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
		kfree(new_ctx);
		mutex_unlock(&criu_freeze_lock);
		return -ESRCH;
	}

	new_ctx->target = target;
	new_ctx->target_generation = generation;
	new_ctx->state = CRIU_FREEZE_FROZEN_SETTLED;
	new_ctx->include_children = include_children;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_FROZEN_SETTLED);
	*ctx = new_ctx;
	mutex_unlock(&criu_freeze_lock);
	return 0;
}

void criu_thaw(struct criu_freeze_ctx *ctx)
{
	if (!ctx)
		return;

	mutex_lock(&criu_freeze_lock);
	if (ctx != criu_freeze_current) {
		mutex_unlock(&criu_freeze_lock);
		return;
	}

	ctx->state = CRIU_FREEZE_THAWING;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_THAWING);
	if (ctx->target)
		put_task_struct(ctx->target);
	criu_freeze_current = NULL;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
	kfree(ctx);
	mutex_unlock(&criu_freeze_lock);
}

bool criu_freeze_settled(struct criu_freeze_ctx *ctx)
{
	bool settled;

	if (!ctx)
		return false;
	mutex_lock(&criu_freeze_lock);
	settled = ctx == criu_freeze_current &&
		ctx->state == CRIU_FREEZE_FROZEN_SETTLED;
	mutex_unlock(&criu_freeze_lock);
	return settled;
}
