/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A2 feasibility probe for the Linux 5.10.29 cgroup-v2 freezer wrapper.
 *
 * This module is deliberately isolated from criu_kernel.ko. Its init path
 * never invokes the probe by default, so building it cannot freeze a task;
 * modpost is the authority on whether an out-of-tree GPL module can resolve
 * the candidate symbols exported by the target kernel.
 */
#include <linux/criu_freezer.h>
#include <linux/module.h>
#include <linux/sched.h>

static bool invoke_probe;
module_param_named(invoke, invoke_probe, bool, 0400);
MODULE_PARM_DESC(invoke,
	"unsafe probe switch; keep false (the module is never loaded by A2 CI)");

static noinline int a2_call_freezer_wrapper(struct task_struct *leader)
{
	struct criu_freezer_cookie *cookie = NULL;
	char original_path[256];
	char temporary_path[256];
	int ret;

	if (!leader)
		return -EINVAL;

	ret = criu_cgroup_freeze_threadgroup(leader, &cookie,
					     original_path, sizeof(original_path),
					     temporary_path, sizeof(temporary_path));
	if (cookie)
		return criu_cgroup_thaw_threadgroup(cookie);
	return ret;
}

static int __init a2_freezer_probe_init(void)
{
	/* Keep references live for modpost without touching a real cgroup. */
	if (invoke_probe)
		return a2_call_freezer_wrapper(current);

	pr_info("A2_FREEZER: probe built; wrapper symbols retained for modpost\n");
	return 0;
}

static void __exit a2_freezer_probe_exit(void)
{
}

module_init(a2_freezer_probe_init);
module_exit(a2_freezer_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A2 cgroup-v2 freezer symbol feasibility probe");
