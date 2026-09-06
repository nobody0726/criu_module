/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>

#include "criu_kernel.h"

static DEFINE_MUTEX(criu_target_lock);
static struct task_struct *criu_target_task;
static u64 criu_target_generation;

int criu_target_set(pid_t pid)
{
	struct task_struct *task;
	struct pid *vpid;

	if (pid <= 0)
		return -EINVAL;
	rcu_read_lock();
	vpid = find_vpid(pid);
	task = pid_task(vpid, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;
	mutex_lock(&criu_target_lock);
	if (criu_target_task)
		put_task_struct(criu_target_task);
	criu_target_task = task;
	criu_target_generation++;
	mutex_unlock(&criu_target_lock);
	return 0;
}

struct task_struct *criu_target_get(u64 *generation)
{
	struct task_struct *task;

	mutex_lock(&criu_target_lock);
	task = criu_target_task;
	if (task)
		get_task_struct(task);
	if (generation)
		*generation = criu_target_generation;
	mutex_unlock(&criu_target_lock);
	return task;
}

void criu_target_clear(void)
{
	mutex_lock(&criu_target_lock);
	if (criu_target_task) {
		put_task_struct(criu_target_task);
		criu_target_task = NULL;
	}
	mutex_unlock(&criu_target_lock);
}
