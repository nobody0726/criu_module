/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include "criu_freezer.h"
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/printk.h>
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
static int criu_freeze_last_error;
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

static int criu_freeze_rollback_locked(struct criu_freeze_ctx *ctx)
{
	int ret;

	ctx->state = CRIU_FREEZE_ROLLBACK;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
	ret = ctx->cgroup_cookie ?
		criu_cgroup_thaw_threadgroup(ctx->cgroup_cookie) : 0;
	if (ret) {
		criu_freeze_last_error = ret;
		return ret;
	}
	criu_freeze_release_tasks(ctx);
	if (ctx->target)
		put_task_struct(ctx->target);
	criu_freeze_current = NULL;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
	kfree(ctx);
	return 0;
}

static const char *criu_freeze_state_name(enum criu_freeze_state state)
{
	switch (state) {
	case CRIU_FREEZE_FREEZING:
		return "freezing";
	case CRIU_FREEZE_FROZEN_SETTLED:
		return "frozen";
	case CRIU_FREEZE_THAWING:
		return "thawing";
	case CRIU_FREEZE_ROLLBACK:
		return "rollback";
	default:
		return "idle";
	}
}

static int criu_freeze_capture_tasks(struct criu_freeze_ctx *ctx)
{
	struct task_struct *thread;
	unsigned int count = 1, i = 0;
	struct criu_freeze_task *tasks;

	rcu_read_lock();
	for_each_thread(ctx->target, thread) {
		/* thread_head includes the leader, which is counted below. */
		if (thread == ctx->target)
			continue;
		count++;
	}
	rcu_read_unlock();

	tasks = kcalloc(count, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;

	rcu_read_lock();
	if (ctx->target->flags & PF_EXITING) {
		pr_info("criu_freeze: capture target exiting pid=%d tgid=%d state=%ld flags=0x%lx\n",
			task_pid_vnr(ctx->target), task_tgid_vnr(ctx->target),
			READ_ONCE(ctx->target->state),
			(unsigned long)READ_ONCE(ctx->target->flags));
		rcu_read_unlock();
		kfree(tasks);
		return -ESRCH;
	}
	tasks[i].task = ctx->target;
	tasks[i].tid = task_pid_vnr(ctx->target);
	tasks[i].stopped = !!(READ_ONCE(ctx->target->state) & __TASK_STOPPED);
	get_task_struct(ctx->target);
	i++;
	for_each_thread(ctx->target, thread) {
		if (thread == ctx->target)
			continue;
		if (i == count)
			break;
		tasks[i].task = thread;
		tasks[i].tid = task_pid_vnr(thread);
		tasks[i].stopped = !!(READ_ONCE(thread->state) & __TASK_STOPPED);
		get_task_struct(thread);
		i++;
	}
	rcu_read_unlock();

	if (i != count) {
		pr_info("criu_freeze: capture thread count changed expected=%u actual=%u target_pid=%d\n",
			count, i, task_pid_vnr(ctx->target));
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
	criu_freeze_last_error = 0;
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
	new_ctx->state = CRIU_FREEZE_FREEZING;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_FREEZING);
	target = criu_target_get(&generation);
	if (!target || task_pid_vnr(target) != vpid) {
		pr_info("criu_freeze: target lookup failed requested=%d target=%p target_pid=%d generation=%llu\n",
			vpid, target, target ? task_pid_vnr(target) : -1,
			generation);
		if (target)
			put_task_struct(target);
		criu_freeze_current = NULL;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
		kfree(new_ctx);
		criu_freeze_last_error = -ESRCH;
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
		criu_freeze_last_error = ret;
		mutex_unlock(&criu_freeze_lock);
		return ret;
	}
	ret = criu_cgroup_freeze_threadgroup(target->group_leader,
					     &new_ctx->cgroup_cookie,
					     new_ctx->original_cgroup,
					     sizeof(new_ctx->original_cgroup),
					     new_ctx->temporary_cgroup,
					     sizeof(new_ctx->temporary_cgroup));
	pr_info("criu_freeze: cgroup freeze pid=%d ret=%d cookie=%p original=%s temporary=%s\n",
		vpid, ret, new_ctx->cgroup_cookie, new_ctx->original_cgroup,
		new_ctx->temporary_cgroup);
	if (ret) {
		criu_freeze_release_tasks(new_ctx);
		put_task_struct(target);
		criu_freeze_current = NULL;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
		kfree(new_ctx);
		criu_freeze_last_error = ret;
		mutex_unlock(&criu_freeze_lock);
		return ret;
	}
	{
		unsigned long deadline = jiffies +
			msecs_to_jiffies(settle_timeout_ms);
		unsigned int settle_loops = 0;

		while (settle_timeout_ms != 0 &&
		       !criu_freeze_settled_locked(new_ctx)) {
			if (!(settle_loops++ % 100))
				pr_info("criu_freeze: settle pending pid=%d state=%ld on_cpu=%d loops=%u remaining=%ld\n",
					vpid, READ_ONCE(new_ctx->tasks[0].task->state),
					READ_ONCE(new_ctx->tasks[0].task->on_cpu), settle_loops,
					(long)(deadline - jiffies));
			if (time_after_eq(jiffies, deadline)) {
				criu_freeze_last_error = -ETIMEDOUT;
				ret = criu_freeze_rollback_locked(new_ctx);
				if (ret) {
					mutex_unlock(&criu_freeze_lock);
					return ret;
				}
				mutex_unlock(&criu_freeze_lock);
				return -ETIMEDOUT;
			}
			mutex_unlock(&criu_freeze_lock);
			msleep(10);
			mutex_lock(&criu_freeze_lock);
		}
		if (settle_timeout_ms == 0) {
			criu_freeze_last_error = -ETIMEDOUT;
			ret = criu_freeze_rollback_locked(new_ctx);
			if (ret) {
				mutex_unlock(&criu_freeze_lock);
				return ret;
			}
			mutex_unlock(&criu_freeze_lock);
			return -ETIMEDOUT;
		}
	}
	new_ctx->state = CRIU_FREEZE_FROZEN_SETTLED;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_FROZEN_SETTLED);
	*ctx = new_ctx;
	mutex_unlock(&criu_freeze_lock);
	return 0;
}

int criu_thaw(struct criu_freeze_ctx *ctx)
{
	int ret;

	mutex_lock(&criu_freeze_lock);
	if (!ctx)
		ctx = criu_freeze_current;
	if (!ctx) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOENT;
	}
	if (ctx != criu_freeze_current) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOENT;
	}

	ctx->state = CRIU_FREEZE_THAWING;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_THAWING);
	if (ctx->cgroup_cookie)
		ret = criu_cgroup_thaw_threadgroup(ctx->cgroup_cookie);
	else
		ret = 0;
	if (ret) {
		criu_freeze_last_error = ret;
		ctx->state = CRIU_FREEZE_ROLLBACK;
		WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_ROLLBACK);
		mutex_unlock(&criu_freeze_lock);
		return ret;
	}
	criu_freeze_release_tasks(ctx);
	if (ctx->target)
		put_task_struct(ctx->target);
	criu_freeze_current = NULL;
	WRITE_ONCE(criu_freeze_current_state, CRIU_FREEZE_IDLE);
	kfree(ctx);
	mutex_unlock(&criu_freeze_lock);
	return 0;
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

int criu_freeze_task_count(struct criu_freeze_ctx *ctx, unsigned int *count)
{
	if (!ctx || !count)
		return -EINVAL;
	mutex_lock(&criu_freeze_lock);
	if (ctx != criu_freeze_current ||
	    ctx->state != CRIU_FREEZE_FROZEN_SETTLED) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOENT;
	}
	*count = ctx->task_count;
	mutex_unlock(&criu_freeze_lock);
	return 0;
}

int criu_freeze_task_get(struct criu_freeze_ctx *ctx, unsigned int index,
			 struct criu_freeze_task_view *view)
{
	if (!ctx || !view)
		return -EINVAL;
	mutex_lock(&criu_freeze_lock);
	if (ctx != criu_freeze_current ||
	    ctx->state != CRIU_FREEZE_FROZEN_SETTLED ||
	    index >= ctx->task_count) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOENT;
	}
	view->task = ctx->tasks[index].task;
	view->tid = ctx->tasks[index].tid;
	view->stopped = ctx->tasks[index].stopped;
	mutex_unlock(&criu_freeze_lock);
	return 0;
}

int criu_freeze_generation(struct criu_freeze_ctx *ctx, u64 *generation)
{
	if (!ctx || !generation)
		return -EINVAL;
	mutex_lock(&criu_freeze_lock);
	if (ctx != criu_freeze_current ||
	    ctx->state != CRIU_FREEZE_FROZEN_SETTLED) {
		mutex_unlock(&criu_freeze_lock);
		return -ENOENT;
	}
	*generation = ctx->target_generation;
	mutex_unlock(&criu_freeze_lock);
	return 0;
}

int criu_freeze_status(struct criu_freeze_status *out)
{
	struct criu_freeze_ctx *ctx;
	unsigned int i;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	mutex_lock(&criu_freeze_lock);
	strscpy(out->state, criu_freeze_state_name(criu_freeze_current_state),
		sizeof(out->state));
	out->last_error = criu_freeze_last_error;
	ctx = criu_freeze_current;
	if (ctx) {
		out->generation = ctx->target_generation;
		out->task_count = ctx->task_count;
		out->settled = ctx->state == CRIU_FREEZE_FROZEN_SETTLED;
		for (i = 0; i < ctx->task_count; i++)
			out->was_stopped |= ctx->tasks[i].stopped;
		strscpy(out->original_cgroup, ctx->original_cgroup,
			sizeof(out->original_cgroup));
		strscpy(out->temporary_cgroup, ctx->temporary_cgroup,
			sizeof(out->temporary_cgroup));
	}
	mutex_unlock(&criu_freeze_lock);
	return 0;
}
