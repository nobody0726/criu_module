/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/criu_freezer.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
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
	struct criu_freeze_task *tasks;
	unsigned int task_count;
	struct criu_freezer_cookie *cgroup_cookie;
	char original_cgroup[CRIU_PATH_MAX];
	char temporary_cgroup[CRIU_PATH_MAX];
};

struct criu_freeze_task {
	struct task_struct *task;
	pid_t tid;
	bool stopped;
};

static DEFINE_MUTEX(criu_freeze_lock);
static struct criu_freeze_ctx *criu_freeze_current;
static enum criu_freeze_state criu_freeze_current_state = CRIU_FREEZE_IDLE;
static unsigned int settle_timeout_ms = 5000;
module_param(settle_timeout_ms, uint, 0644);

static bool criu_freeze_settled_locked(struct criu_freeze_ctx *ctx)
{
	unsigned int i;

	if (ctx != criu_freeze_current ||
	    ctx->state != CRIU_FREEZE_FREEZING)
		return false;
	for (i = 0; i < ctx->task_count; i++)
		if (READ_ONCE(ctx->tasks[i].task->state) == TASK_RUNNING)
			return false;
	return true;
}

static void criu_freeze_release_tasks(struct criu_freeze_ctx *ctx)
{
	unsigned int i;

	if (!ctx || !ctx->tasks)
		return;
	for (i = 0; i < ctx->task_count; i++)
		if (ctx->tasks[i].task)
			put_task_struct(ctx->tasks[i].task);
	kfree(ctx->tasks);
	ctx->tasks = NULL;
	ctx->task_count = 0;
}

static int criu_freeze_capture_tasks(struct criu_freeze_ctx *ctx)
{
	struct task_struct *thread;
	unsigned int count = 1, i = 0;
	struct criu_freeze_task *tasks;

	read_lock(&tasklist_lock);
	for_each_thread(ctx->target, thread)
		count++;
	read_unlock(&tasklist_lock);

	tasks = kcalloc(count, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;

	read_lock(&tasklist_lock);
	if (ctx->target->flags & PF_EXITING) {
		read_unlock(&tasklist_lock);
		kfree(tasks);
		return -ESRCH;
	}
	tasks[i].task = ctx->target;
	tasks[i].tid = task_pid_vnr(ctx->target);
	tasks[i].stopped = !!(READ_ONCE(ctx->target->state) & __TASK_STOPPED);
	get_task_struct(ctx->target);
	i++;
	for_each_thread(ctx->target, thread) {
		if (i == count)
			break;
		tasks[i].task = thread;
		tasks[i].tid = task_pid_vnr(thread);
		tasks[i].stopped = !!(READ_ONCE(thread->state) & __TASK_STOPPED);
		get_task_struct(thread);
		i++;
	}
	read_unlock(&tasklist_lock);

	if (i != count) {
		while (i)
			put_task_struct(tasks[--i].task);
		kfree(tasks);
		return -ESRCH;
	}
	ctx->tasks = tasks;
	ctx->task_count = count;
	return 0;
}

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
	int ret;

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
	if (target != target->group_leader) {
		struct task_struct *leader = target->group_leader;

		get_task_struct(leader);
		put_task_struct(target);
		target = leader;
	}

	new_ctx->target = target;
	new_ctx->target_generation = generation;
	new_ctx->include_children = include_children;
	ret = criu_freeze_capture_tasks(new_ctx);
	if (ret) {
		put_task_struct(target);
		criu_freeze_current = NULL;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
		kfree(new_ctx);
		mutex_unlock(&criu_freeze_lock);
		return ret;
	}
	ret = criu_cgroup_freeze_threadgroup(target->group_leader,
					     &new_ctx->cgroup_cookie,
					     new_ctx->original_cgroup,
					     sizeof(new_ctx->original_cgroup),
					     new_ctx->temporary_cgroup,
					     sizeof(new_ctx->temporary_cgroup));
	if (ret) {
		criu_freeze_release_tasks(new_ctx);
		put_task_struct(target);
		criu_freeze_current = NULL;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
		kfree(new_ctx);
		mutex_unlock(&criu_freeze_lock);
		return ret;
	}
	{
		unsigned long deadline = jiffies +
			msecs_to_jiffies(settle_timeout_ms);

		while (!criu_freeze_settled_locked(new_ctx)) {
			if (time_after_eq(jiffies, deadline)) {
				criu_cgroup_thaw_threadgroup(new_ctx->cgroup_cookie);
				criu_freeze_release_tasks(new_ctx);
				put_task_struct(target);
				criu_freeze_current = NULL;
				WRITE_ONCE(criu_freeze_current_state,
					   CRIU_FREEZE_ROLLBACK);
				WRITE_ONCE(criu_freeze_current_state,
					   CRIU_FREEZE_IDLE);
				kfree(new_ctx);
				mutex_unlock(&criu_freeze_lock);
				return -ETIMEDOUT;
			}
			mutex_unlock(&criu_freeze_lock);
			msleep(10);
			mutex_lock(&criu_freeze_lock);
		}
	}
	new_ctx->state = CRIU_FREEZE_FROZEN_SETTLED;
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
	if (ctx->cgroup_cookie)
		criu_cgroup_thaw_threadgroup(ctx->cgroup_cookie);
	criu_freeze_release_tasks(ctx);
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
	if (settled) {
		unsigned int i;

		for (i = 0; i < ctx->task_count; i++) {
			if (READ_ONCE(ctx->tasks[i].task->state) == TASK_RUNNING) {
				settled = false;
				break;
			}
		}
	}
	mutex_unlock(&criu_freeze_lock);
	return settled;
}
