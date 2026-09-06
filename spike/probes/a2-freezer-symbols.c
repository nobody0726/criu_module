// SPDX-License-Identifier: GPL-2.0
/*
 * A2 feasibility probe for the Linux 5.10.29 cgroup-v2 freezer API.
 *
 * This module is deliberately isolated from criu_kernel.ko. Its init path
 * never invokes the probe by default, so building it cannot freeze a task;
 * modpost is the authority on whether an out-of-tree GPL module can resolve
 * the candidate symbols exported by the target kernel.
 */
#include <linux/cgroup.h>
#include <linux/module.h>
#include <linux/sched.h>

static bool invoke_probe;
module_param_named(invoke, invoke_probe, bool, 0400);
MODULE_PARM_DESC(invoke,
	"unsafe probe switch; keep false (the module is never loaded by A2 CI)");

static int a2_call_freezer_symbols(struct cgroup *cgrp,
					struct task_struct *task)
{
	if (!cgrp || !task)
		return -EINVAL;

	/* These are the exact freezer operations in kernel/cgroup/freezer.c. */
	cgroup_freeze(cgrp, true);
	cgroup_freezer_migrate_task(task, cgrp, cgrp);
	cgroup_enter_frozen();
	cgroup_leave_frozen(false);
	return 0;
}

static int __init a2_freezer_probe_init(void)
{
	/* Keep references live for modpost without touching a real cgroup. */
	if (invoke_probe)
		return a2_call_freezer_symbols(NULL, current);

	pr_info("A2_FREEZER: probe built; candidate symbols retained for modpost\n");
	return 0;
}

static void __exit a2_freezer_probe_exit(void)
{
}

module_init(a2_freezer_probe_init);
module_exit(a2_freezer_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A2 cgroup-v2 freezer symbol feasibility probe");
