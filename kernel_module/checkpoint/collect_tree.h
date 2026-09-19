/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_COLLECT_TREE_H
#define CRIU_COLLECT_TREE_H

#include <linux/sched.h>
#include <linux/types.h>

#define CRIU_A7_MAX_PROCESSES 1024U
#define CRIU_A7_MAX_TASKS_PER_PROCESS 1024U

struct criu_tree_task {
	struct task_struct *task;
	pid_t tid;
	bool stopped;
};

struct criu_tree_process {
	struct task_struct *leader;
	struct task_struct *parent;
	struct criu_tree_task *tasks;
	unsigned int task_count;
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	bool root;
	bool external_parent;
	bool session_leader;
	bool process_group_leader;
};

struct criu_tree_closure {
	struct pid_namespace *namespace;
	struct criu_tree_process *processes;
	unsigned int process_count;
};

int criu_collect_tree(struct task_struct *target,
		      struct criu_tree_closure **out);
void criu_tree_closure_free(struct criu_tree_closure *tree);
int criu_tree_process_leaders(struct criu_tree_closure *tree,
			      struct task_struct ***leaders);

#endif
