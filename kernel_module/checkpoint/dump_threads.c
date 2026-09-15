/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../../include/criu_snapshot.h"
#include "dump_threads.h"

struct criu_pinned_thread {
	struct task_struct *task;
	pid_t tid;
};

static unsigned int thread_count(struct task_struct *leader)
{
	struct task_struct *thread;
	unsigned int count = 1;

	rcu_read_lock();
	for_each_thread(leader, thread)
		if (thread != leader)
			count++;
	rcu_read_unlock();
	return count;
}

static int pin_threads(struct task_struct *leader,
			struct criu_pinned_thread *threads,
			unsigned int expected)
{
	struct task_struct *thread;
	unsigned int i = 0;

	rcu_read_lock();
	get_task_struct(leader);
	threads[i].task = leader;
	threads[i].tid = task_pid_vnr(leader);
	i++;
	for_each_thread(leader, thread) {
		if (thread == leader)
			continue;
		if (i == expected)
			break;
		get_task_struct(thread);
		threads[i].task = thread;
		threads[i].tid = task_pid_vnr(thread);
		i++;
	}
	rcu_read_unlock();
	if (i == expected)
		return 0;
	while (i)
		put_task_struct(threads[--i].task);
	return -EAGAIN;
}

static int write_thread(struct criu_pinned_thread *pinned,
			struct criu_snapshot_writer *writer)
{
	struct criu_snapshot_thread_record record;
	struct pt_regs *regs;

	if (!pinned || !pinned->task || !writer)
		return -EINVAL;
	if (READ_ONCE(pinned->task->flags) & PF_EXITING)
		return -ESRCH;
	regs = task_pt_regs(pinned->task);
	if (!regs || sizeof(*regs) > sizeof(record.regs))
		return -EOPNOTSUPP;
	memset(&record, 0, sizeof(record));
	record.tid = pinned->tid;
	record.tgid = task_tgid_vnr(pinned->task);
	record.regs_size = sizeof(*regs);
	memcpy(record.blocked, &pinned->task->blocked,
	       min(sizeof(record.blocked), sizeof(pinned->task->blocked)));
	memcpy(record.regs, regs, sizeof(*regs));
#ifdef CONFIG_ARM64
	record.tls = pinned->task->thread.uw.tp_value;
#endif
	return criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_THREAD,
					   0, &record, sizeof(record));
}

int criu_dump_threads(struct task_struct *leader,
			struct criu_snapshot_writer *writer)
{
	struct criu_pinned_thread *threads;
	unsigned int expected;
	unsigned int i;
	int ret;

	if (!leader || !writer)
		return -EINVAL;
	expected = thread_count(leader);
	threads = kcalloc(expected, sizeof(*threads), GFP_KERNEL);
	if (!threads)
		return -ENOMEM;
	ret = pin_threads(leader, threads, expected);
	if (ret)
		goto out;
	if (thread_count(leader) != expected) {
		ret = -EAGAIN;
		goto release;
	}
	for (i = 0; i < expected; i++) {
		ret = write_thread(&threads[i], writer);
		if (ret)
			break;
	}
release:
	for (i = 0; i < expected; i++)
		put_task_struct(threads[i].task);
out:
	kfree(threads);
	return ret;
}
