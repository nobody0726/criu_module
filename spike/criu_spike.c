// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "criu_spike.h"

struct criu_spike_args criu_spike_args = {
	.probe = CRIU_SPIKE_PROBE,
};

static struct dentry *criu_spike_root;
static struct pid *criu_spike_pid;
static struct task_struct *criu_spike_task;
static struct mm_struct *criu_spike_mm;
static struct page *criu_spike_page;
static char criu_spike_status[CRIU_SPIKE_STATUS_MAX];
static char criu_spike_report[CRIU_SPIKE_REPORT_MAX];
static char criu_spike_vmas[CRIU_SPIKE_VMAS_MAX];

module_param_named(target_pid, criu_spike_args.target_pid, int, 0444);
module_param_named(anon_addr, criu_spike_args.anon_addr, ulong, 0444);
module_param_named(shared_addr, criu_spike_args.shared_addr, ulong, 0444);
module_param_named(guard_addr, criu_spike_args.guard_addr, ulong, 0444);
module_param_named(insert_addr, criu_spike_args.insert_addr, ulong, 0444);
module_param_string(probe, criu_spike_args.probe,
			    sizeof(criu_spike_args.probe), 0444);

static const char * const criu_spike_probe_ids[] = {
	"1.1", "1.2", "1.3", "1.4",
	"2.1", "2.2", "2.3", "2.4", "2.5",
	"3.1", "3.2", "3.3", "3.4", "3.5",
	"4.1", "4.2", "4.3", "4.4a", "4.4b",
	"4.5a", "4.5b", "4.6", "4.7", "4.8",
};

static bool criu_spike_valid_probe(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(criu_spike_probe_ids); i++) {
		if (!strcmp(criu_spike_args.probe, criu_spike_probe_ids[i]))
			return true;
	}
	return false;
}

static bool criu_spike_valid_user_page(unsigned long address)
{
	if (!address || !PAGE_ALIGNED(address))
		return false;
	if (address + PAGE_SIZE < address)
		return false;
	return access_ok((void __user *)address, PAGE_SIZE);
}

static int criu_spike_validate_args(void)
{
	if (!criu_spike_valid_probe())
		return -EINVAL;
	if (criu_spike_args.target_pid <= 0)
		return -EINVAL;
	if (!criu_spike_valid_user_page(criu_spike_args.anon_addr) ||
	    !criu_spike_valid_user_page(criu_spike_args.shared_addr) ||
	    !criu_spike_valid_user_page(criu_spike_args.guard_addr) ||
	    !criu_spike_valid_user_page(criu_spike_args.insert_addr))
		return -EFAULT;
	return 0;
}

static void criu_spike_release_resources(void)
{
	if (criu_spike_page) {
		put_page(criu_spike_page);
		criu_spike_page = NULL;
	}
	if (criu_spike_mm) {
		mmput(criu_spike_mm);
		criu_spike_mm = NULL;
	}
	if (criu_spike_task) {
		put_task_struct(criu_spike_task);
		criu_spike_task = NULL;
	}
	if (criu_spike_pid) {
		put_pid(criu_spike_pid);
		criu_spike_pid = NULL;
	}
	if (criu_spike_root) {
		debugfs_remove_recursive(criu_spike_root);
		criu_spike_root = NULL;
	}
}

static int criu_spike_prepare_resources(void)
{
	criu_spike_pid = find_get_pid(criu_spike_args.target_pid);
	if (!criu_spike_pid)
		return -ESRCH;

	criu_spike_task = get_pid_task(criu_spike_pid, PIDTYPE_PID);
	if (!criu_spike_task)
		return -ESRCH;

	criu_spike_mm = get_task_mm(criu_spike_task);
	if (!criu_spike_mm)
		return -ESRCH;

	return 0;
}

static void criu_spike_prepare_output(void)
{
	scnprintf(criu_spike_status, sizeof(criu_spike_status),
		  "protocol=1\nstate=READY\nresult=NOT_IMPLEMENTED\n");
	scnprintf(criu_spike_report, sizeof(criu_spike_report),
		  "protocol=1\n"
		  "probe=%s\n"
		  "target_pid=%d\n"
		  "anon_addr=0x%lx\n"
		  "shared_addr=0x%lx\n"
		  "guard_addr=0x%lx\n"
		  "insert_addr=0x%lx\n"
		  "target_valid=1\n"
		  "result=NOT_IMPLEMENTED\n"
		  "reason=Task 3 framework only\n",
		  criu_spike_args.probe, criu_spike_args.target_pid,
		  criu_spike_args.anon_addr, criu_spike_args.shared_addr,
		  criu_spike_args.guard_addr, criu_spike_args.insert_addr);
	scnprintf(criu_spike_vmas, sizeof(criu_spike_vmas),
		  "protocol=1\nprobe=%s\nresult=NOT_IMPLEMENTED\n"
		  "vmas=NOT_IMPLEMENTED\n", criu_spike_args.probe);
}

static int criu_spike_status_show(struct seq_file *m, void *unused)
{
	seq_puts(m, criu_spike_status);
	return 0;
}

static int criu_spike_report_show(struct seq_file *m, void *unused)
{
	seq_puts(m, criu_spike_report);
	return 0;
}

static int criu_spike_vmas_show(struct seq_file *m, void *unused)
{
	seq_puts(m, criu_spike_vmas);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(criu_spike_status);
DEFINE_SHOW_ATTRIBUTE(criu_spike_report);
DEFINE_SHOW_ATTRIBUTE(criu_spike_vmas);

static int __init criu_spike_init(void)
{
	int ret;

	ret = criu_spike_validate_args();
	if (ret)
		return ret;

	ret = criu_spike_prepare_resources();
	if (ret)
		goto fail;

	criu_spike_root = debugfs_create_dir("criu_spike", NULL);
	if (IS_ERR_OR_NULL(criu_spike_root)) {
		ret = criu_spike_root ? PTR_ERR(criu_spike_root) : -ENOMEM;
		criu_spike_root = NULL;
		goto fail;
	}

	if (IS_ERR_OR_NULL(debugfs_create_file("status", 0444, criu_spike_root,
					      NULL, &criu_spike_status_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("report", 0444, criu_spike_root,
					      NULL, &criu_spike_report_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("vmas", 0444, criu_spike_root,
					      NULL, &criu_spike_vmas_fops))) {
		ret = -ENOMEM;
		goto fail;
	}

	criu_spike_prepare_output();

	pr_info("criu_spike: framework loaded for probe %s\n",
		criu_spike_args.probe);
	return 0;

fail:
	criu_spike_release_resources();
	return ret;
}

static void __exit criu_spike_exit(void)
{
	criu_spike_release_resources();
	pr_info("criu_spike: framework unloaded for probe %s\n",
		criu_spike_args.probe);
}

module_init(criu_spike_init);
module_exit(criu_spike_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-shot CRIU S0 feasibility probe framework");
