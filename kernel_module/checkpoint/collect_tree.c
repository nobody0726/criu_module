/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

#include "collect_tree.h"

static int tree_index(const struct criu_tree_closure *tree,
		      struct task_struct *leader)
{
	unsigned int i;

	for (i = 0; i < tree->process_count; i++)
		if (tree->processes[i].leader == leader)
			return (int)i;
	return -1;
}

static void tree_release_process(struct criu_tree_process *process)
{
	unsigned int i;

	if (!process)
		return;
	for (i = 0; i < process->task_count; i++)
		if (process->tasks[i].task)
			put_task_struct(process->tasks[i].task);
	if (!process->task_count && process->leader)
		put_task_struct(process->leader);
	kfree(process->tasks);
	process->leader = NULL;
	process->tasks = NULL;
	process->task_count = 0;
}

void criu_tree_closure_free(struct criu_tree_closure *tree)
{
	unsigned int i;

	if (!tree)
		return;
	for (i = 0; i < tree->process_count; i++)
		tree_release_process(&tree->processes[i]);
	kfree(tree->processes);
	if (tree->namespace)
		put_pid_ns(tree->namespace);
	kfree(tree);
}

static int tree_capture_threads(struct criu_tree_process *process)
{
	struct task_struct *thread;
	unsigned int count = 1, i = 0;
	struct criu_tree_task *tasks;

	rcu_read_lock();
	for_each_thread(process->leader, thread)
		if (thread != process->leader)
			count++;
	rcu_read_unlock();
	if (!count || count > CRIU_A7_MAX_TASKS_PER_PROCESS)
		return -EOPNOTSUPP;
	tasks = kcalloc(count, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;

	rcu_read_lock();
	if (READ_ONCE(process->leader->flags) & PF_EXITING) {
		rcu_read_unlock();
		kfree(tasks);
		return -ESRCH;
	}
	tasks[i].task = process->leader;
	tasks[i].tid = task_pid_vnr(process->leader);
	tasks[i].stopped = !!(READ_ONCE(process->leader->state) & __TASK_STOPPED);
	i++;
	for_each_thread(process->leader, thread) {
		if (thread == process->leader)
			continue;
		if (i == count)
			break;
		if (READ_ONCE(thread->flags) & PF_EXITING)
			break;
		tasks[i].task = thread;
		tasks[i].tid = task_pid_vnr(thread);
		tasks[i].stopped = !!(READ_ONCE(thread->state) & __TASK_STOPPED);
		get_task_struct(thread);
		i++;
	}
	rcu_read_unlock();
	if (i != count) {
		while (i)
			put_task_struct(tasks[--i].task);
		process->leader = NULL;
		kfree(tasks);
		return -EAGAIN;
	}
	/* The leader reference is transferred from the process node to tasks[0]. */
	process->leader = tasks[0].task;
	process->tasks = tasks;
	process->task_count = count;
	return 0;
}

static int tree_validate_topology(struct criu_tree_closure *tree,
				  struct task_struct *target)
{
	unsigned int i, root_count = 0;
	struct pid_namespace *ns = tree->namespace;

	for (i = 0; i < tree->process_count; i++) {
		struct criu_tree_process *process = &tree->processes[i];
		struct task_struct *parent;
		int parent_index;

		process->pid = pid_nr_ns(task_pid(process->leader), ns);
		process->tgid = pid_nr_ns(task_tgid(process->leader), ns);
		process->pgid = task_pgrp_vnr(process->leader);
		process->sid = task_session_vnr(process->leader);
		process->born_sid = -1;
		process->session_leader = process->pid == process->sid;
		process->process_group_leader = process->pid == process->pgid;
		if (!process->pid || process->pid != process->tgid ||
		    process->leader != process->leader->group_leader ||
		    READ_ONCE(process->leader->flags) & PF_EXITING)
			return -ESRCH;

		rcu_read_lock();
		parent = rcu_dereference(process->leader->real_parent);
		parent = parent ? parent->group_leader : NULL;
		if (parent && task_active_pid_ns(parent) != ns) {
			rcu_read_unlock();
			return -EOPNOTSUPP;
		}
		parent_index = tree_index(tree, parent);
		rcu_read_unlock();
		if (process->leader == target) {
			process->root = true;
			root_count++;
			if (parent_index >= 0) {
				process->parent = tree->processes[parent_index].leader;
				process->external_parent = false;
			} else {
				process->parent = NULL;
				process->external_parent = true;
			}
			process->ppid = 0;
		} else {
			if (parent_index < 0)
				return -EOPNOTSUPP;
			process->parent = tree->processes[parent_index].leader;
			process->ppid = tree->processes[parent_index].pid;
			if (process->sid != tree->processes[parent_index].sid &&
			    !process->session_leader)
				process->born_sid = process->sid;
		}
	}
	if (root_count != 1)
		return -EUCLEAN;
	for (i = 0; i < tree->process_count; i++) {
		{
			unsigned int j;
			bool pgid_leader = false, sid_leader = false;
			for (j = 0; j < tree->process_count; j++) {
				pgid_leader |= tree->processes[j].pid ==
					tree->processes[i].pgid;
				sid_leader |= tree->processes[j].pid ==
					tree->processes[i].sid;
			}
			if (!pgid_leader || !sid_leader)
				return -EOPNOTSUPP;
		}
	}
	return 0;
}

int criu_collect_tree(struct task_struct *target,
		      struct criu_tree_closure **out)
{
	struct criu_tree_closure *tree;
	struct task_struct **stack;
	unsigned int stack_count = 0;
	struct pid_namespace *ns;
	int ret = 0;

	if (!target || !out || target != target->group_leader)
		return -EINVAL;
	*out = NULL;
	ns = task_active_pid_ns(target);
	if (!ns || ns != task_active_pid_ns(current))
		return -EOPNOTSUPP;
	tree = kzalloc(sizeof(*tree), GFP_KERNEL);
	if (!tree)
		return -ENOMEM;
	tree->namespace = get_pid_ns(ns);
	tree->processes = kcalloc(CRIU_A7_MAX_PROCESSES,
				  sizeof(*tree->processes), GFP_KERNEL);
	if (!tree->processes) {
		criu_tree_closure_free(tree);
		return -ENOMEM;
	}
	stack = kcalloc(CRIU_A7_MAX_PROCESSES, sizeof(*stack), GFP_KERNEL);
	if (!stack) {
		criu_tree_closure_free(tree);
		return -ENOMEM;
	}
	stack[stack_count++] = target;
	while (stack_count) {
		struct task_struct *leader = stack[--stack_count];
		struct task_struct *child;
		struct criu_tree_process *process;
		leader = leader->group_leader;
		if (tree_index(tree, leader) >= 0)
			continue;
		if (tree->process_count == CRIU_A7_MAX_PROCESSES) {
			ret = -E2BIG;
			goto fail;
		}
		process = &tree->processes[tree->process_count++];
		process->leader = leader;
		get_task_struct(leader);
		rcu_read_lock();
		list_for_each_entry_rcu(child, &leader->children, sibling) {
			struct task_struct *child_leader = child->group_leader;

			if (task_active_pid_ns(child_leader) != ns) {
				ret = -EOPNOTSUPP;
				break;
			}
			if (tree_index(tree, child_leader) < 0) {
				if (stack_count == CRIU_A7_MAX_PROCESSES) {
					ret = -E2BIG;
					break;
				}
				stack[stack_count++] = child_leader;
			}
		}
		rcu_read_unlock();
		if (ret)
			goto fail;
	}
	{
		unsigned int i;
		for (i = 0; i < tree->process_count; i++) {
			ret = tree_capture_threads(&tree->processes[i]);
			if (ret)
				goto fail;
		}
	}
	ret = tree_validate_topology(tree, target);
	if (ret)
		goto fail;
	*out = tree;
	kfree(stack);
	return 0;
fail:
	kfree(stack);
	criu_tree_closure_free(tree);
	return ret;
}

int criu_tree_process_leaders(struct criu_tree_closure *tree,
			      struct task_struct ***leaders)
{
	struct task_struct **result;
	unsigned int i;

	if (!tree || !leaders || !tree->process_count)
		return -EINVAL;
	result = kcalloc(tree->process_count, sizeof(*result), GFP_KERNEL);
	if (!result)
		return -ENOMEM;
	for (i = 0; i < tree->process_count; i++)
		result[i] = tree->processes[i].leader;
	*leaders = result;
	return 0;
}
