/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/capability.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "criu_kernel.h"

static struct dentry *criu_root;

struct criu_view {
	struct task_struct *task;
	u64 generation;
	struct criu_snapshot snapshot;
};

static const char *special_name(enum criu_vma_special special)
{
	switch (special) {
	case CRIU_VMA_SPECIAL_VDSO: return "VDSO";
	case CRIU_VMA_SPECIAL_VVAR: return "VVAR";
	case CRIU_VMA_SPECIAL_HUGETLB: return "HUGETLB";
	case CRIU_VMA_SPECIAL_DEVICE: return "DEVICE";
	case CRIU_VMA_SPECIAL_PFNMAP: return "PFNMAP";
	case CRIU_VMA_SPECIAL_MIXEDMAP: return "MIXEDMAP";
	case CRIU_VMA_SPECIAL_PROT_NONE: return "PROT_NONE";
	case CRIU_VMA_SPECIAL_UNKNOWN: return "UNKNOWN";
	default: return "NONE";
	}
}

static const char *class_name(enum criu_vma_class class)
{
	switch (class) {
	case CRIU_VMA_ANON_PRIVATE: return "ANON_PRIVATE";
	case CRIU_VMA_ANON_SHARED: return "ANON_SHARED";
	case CRIU_VMA_FILE_SHARED: return "FILE_SHARED";
	case CRIU_VMA_FILE_PRIVATE: return "FILE_PRIVATE";
	default: return "UNSUPPORTED";
	}
}

static void prot_string(const struct criu_vma_info *vma, char *buf)
{
	buf[0] = vma->prot & VM_READ ? 'r' : '-';
	buf[1] = vma->prot & VM_WRITE ? 'w' : '-';
	buf[2] = vma->prot & VM_EXEC ? 'x' : '-';
	buf[3] = vma->shared ? 's' : 'p';
	buf[4] = '\0';
}

static int view_open(struct inode *inode, struct file *file, bool samples,
			     int (*show)(struct seq_file *, void *))
{
	struct criu_view *view;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	view = kzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return -ENOMEM;
	view->task = criu_target_get(&view->generation);
	if (!view->task) {
		kfree(view);
		return -ESRCH;
	}
	ret = criu_snapshot_capture(view->task, &view->snapshot, samples);
	if (ret) {
		put_task_struct(view->task);
		kfree(view);
		return ret;
	}
	ret = single_open(file, show, view);
	if (ret) {
		criu_snapshot_destroy(&view->snapshot);
		put_task_struct(view->task);
		kfree(view);
	}
	return ret;
}

static int view_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct criu_view *view = seq ? seq->private : NULL;

	if (view) {
		criu_snapshot_destroy(&view->snapshot);
		put_task_struct(view->task);
		kfree(view);
	}
	return single_release(inode, file);
}

static int task_show(struct seq_file *m, void *unused)
{
	struct criu_view *view = m->private;
	struct criu_mm_info *mm = &view->snapshot.mm;

	seq_printf(m, "generation=%llu pid=%d tgid=%d comm=%s state=%lu "
		   "vma_count=%lu special_count=%lu unsupported_count=%lu "
		   "total_vm=%lu start_code=%lx end_code=%lx start_data=%lx "
		   "end_data=%lx start_brk=%lx brk=%lx start_stack=%lx "
		   "arg_start=%lx arg_end=%lx env_start=%lx env_end=%lx\n",
		   view->generation, mm->pid, mm->tgid, mm->comm, mm->state,
		   mm->vma_count, mm->special_count, mm->unsupported_count,
		   mm->total_vm, mm->start_code, mm->end_code, mm->start_data,
		   mm->end_data, mm->start_brk, mm->brk, mm->start_stack,
		   mm->arg_start, mm->arg_end, mm->env_start, mm->env_end);
	return 0;
}

static int maps_show(struct seq_file *m, void *unused)
{
	struct criu_view *view = m->private;
	unsigned long i;

	for (i = 0; i < view->snapshot.mm.vma_count; i++) {
		struct criu_vma_info *vma = &view->snapshot.vmas[i];
		char perms[5];

		prot_string(vma, perms);
		seq_printf(m, "%lx-%lx %s %08lx %02x:%02x %-lu %s\n",
			   vma->start, vma->end, perms,
			   vma->ino ? vma->pgoff << PAGE_SHIFT : 0,
			   MAJOR(vma->dev), MINOR(vma->dev), vma->ino,
			   vma->path[0] ? vma->path : "-");
	}
	return 0;
}

static int vmas_ext_show(struct seq_file *m, void *unused)
{
	struct criu_view *view = m->private;
	unsigned long i;

	for (i = 0; i < view->snapshot.mm.vma_count; i++) {
		struct criu_vma_info *vma = &view->snapshot.vmas[i];
		const char *sample_status = "SKIPPED";
		const char *path_status = "OK";
		char prot[5];

		if (vma->sample_status == CRIU_SAMPLE_OK)
			sample_status = "OK";
		else if (vma->sample_status == CRIU_SAMPLE_UNAVAILABLE)
			sample_status = "UNAVAILABLE";
		if (vma->path_status == CRIU_PATH_TRUNCATED)
			path_status = "TRUNCATED";
		else if (vma->path_status == CRIU_PATH_ERROR)
			path_status = "ERROR";
		prot_string(vma, prot);
		seq_printf(m, "generation=%llu start=%lx end=%lx pgoff=%lx "
			   "prot=%s shared=%u class=%s special=%s dump_policy=%s "
			   "growsdown=%u dontdump=%u locked=%u vm_flags=%lx "
			   "dev=%u:%u ino=%lu path_status=%s sample_status=%s "
			   "sample=0x%02x path=%s\n", view->generation,
			   vma->start, vma->end, vma->pgoff, prot, vma->shared,
			   class_name(vma->class), special_name(vma->special),
			   vma->dontdump ? "SKIP" : "ELIGIBLE", vma->growsdown,
			   vma->dontdump, vma->locked, vma->vm_flags_raw,
			   MAJOR(vma->dev), MINOR(vma->dev), vma->ino, path_status,
			   sample_status, vma->sample, vma->path[0] ? vma->path : "-");
	}
	return 0;
}

static int target_show(struct seq_file *m, void *unused)
{
	struct task_struct *task;
	u64 generation;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	task = criu_target_get(&generation);
	if (!task)
		return -ESRCH;
	seq_printf(m, "generation=%llu pid=%d tgid=%d comm=%s\n", generation,
		   task_pid_nr(task), task_tgid_nr(task), task->comm);
	put_task_struct(task);
	return 0;
}

static int target_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return single_open(file, target_show, NULL);
}

static ssize_t target_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *pos)
{
	char input[32];
	int pid, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtoint(input, 10, &pid);
	if (ret)
		return -EINVAL;
	ret = criu_target_set(pid);
	return ret ? ret : count;
}

static ssize_t freeze_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *pos)
{
	char input[16];
	unsigned long value;
	struct criu_freeze_ctx *ctx;
	struct task_struct *target;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtoul(input, 10, &value);
	if (ret || value != 1)
		return -EINVAL;
	target = criu_target_get(NULL);
	if (!target)
		return -ESRCH;
	ret = criu_freeze(task_pid_vnr(target), false, &ctx);
	put_task_struct(target);
	return ret ? ret : count;
}

static ssize_t thaw_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *pos)
{
	char input[16];
	unsigned long value;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtoul(input, 10, &value);
	if (ret || value != 1)
		return -EINVAL;
	/*
	 * The active context is deliberately owned by the module; a NULL
	 * argument means "the current context" for this control operation.
	 */
	ret = criu_thaw(NULL);
	return ret ? ret : count;
}

static const struct file_operations freeze_fops = {
	.owner = THIS_MODULE,
	.write = freeze_write,
};

static const struct file_operations thaw_fops = {
	.owner = THIS_MODULE,
	.write = thaw_write,
};

static ssize_t criu_view_read(struct file *file, char __user *buf,
				      size_t size, loff_t *ppos)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return seq_read(file, buf, size, ppos);
}

static loff_t criu_view_llseek(struct file *file, loff_t offset, int whence)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return seq_lseek(file, offset, whence);
}

static const struct file_operations target_fops = {
	.owner = THIS_MODULE,
	.open = target_open,
	.read = criu_view_read,
	.llseek = criu_view_llseek,
	.release = single_release,
	.write = target_write,
};

static int task_open(struct inode *inode, struct file *file)
{
	return view_open(inode, file, false, task_show);
}

static int maps_open(struct inode *inode, struct file *file)
{
	return view_open(inode, file, false, maps_show);
}

static int vmas_ext_open(struct inode *inode, struct file *file)
{
	return view_open(inode, file, true, vmas_ext_show);
}

static const struct file_operations task_fops = {
	.owner = THIS_MODULE,
	.open = task_open,
	.read = criu_view_read,
	.llseek = criu_view_llseek,
	.release = view_release,
};

static const struct file_operations maps_fops = {
	.owner = THIS_MODULE,
	.open = maps_open,
	.read = criu_view_read,
	.llseek = criu_view_llseek,
	.release = view_release,
};

static const struct file_operations vmas_ext_fops = {
	.owner = THIS_MODULE,
	.open = vmas_ext_open,
	.read = criu_view_read,
	.llseek = criu_view_llseek,
	.release = view_release,
};

static int criu_status_show(struct seq_file *m, void *unused)
{
	struct criu_freeze_status status;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	seq_puts(m, "criu_kernel:ok\n");
	if (criu_freeze_status(&status))
		return -EIO;
	seq_printf(m, "freeze_state=%s freeze_generation=%llu "
		   "freeze_task_count=%u freeze_settled=%u "
		   "freeze_was_stopped=%u freeze_last_error=%d "
		   "freeze_cgroup_original=%s freeze_cgroup_temporary=%s\n",
		   status.state, status.generation, status.task_count,
		   status.settled, status.was_stopped, status.last_error,
		   status.original_cgroup[0] ? status.original_cgroup : "unavailable",
		   status.temporary_cgroup[0] ? status.temporary_cgroup : "unavailable");
	return 0;
}

static int criu_status_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return single_open(file, criu_status_show, NULL);
}

static ssize_t criu_status_read(struct file *file, char __user *buf,
				 size_t size, loff_t *ppos)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return seq_read(file, buf, size, ppos);
}

static const struct file_operations criu_status_fops = {
	.owner = THIS_MODULE,
	.open = criu_status_open,
	.read = criu_status_read,
	.llseek = criu_view_llseek,
	.release = single_release,
};

static int __init criu_init(void)
{
	struct dentry *entry;

	criu_root = debugfs_create_dir("criu", NULL);
	if (IS_ERR_OR_NULL(criu_root))
		return criu_root ? PTR_ERR(criu_root) : -ENOMEM;
	entry = debugfs_create_file("status", 0400, criu_root, NULL,
				    &criu_status_fops);
	if (IS_ERR_OR_NULL(entry)) {
		debugfs_remove_recursive(criu_root);
		return entry ? PTR_ERR(entry) : -ENOMEM;
	}
	if (!debugfs_create_file("target", 0600, criu_root, NULL,
				&target_fops) ||
	    !debugfs_create_file("freeze", 0200, criu_root, NULL,
				 &freeze_fops) ||
	    !debugfs_create_file("thaw", 0200, criu_root, NULL,
				 &thaw_fops) ||
	    !debugfs_create_file("task", 0400, criu_root, NULL, &task_fops) ||
	    !debugfs_create_file("maps", 0400, criu_root, NULL, &maps_fops) ||
	    !debugfs_create_file("vmas_ext", 0400, criu_root, NULL,
				 &vmas_ext_fops)) {
		debugfs_remove_recursive(criu_root);
		return -ENOMEM;
	}
	pr_info("criu_kernel: loaded\n");
	return 0;
}

static void __exit criu_exit(void)
{
	criu_thaw(NULL);
	criu_target_clear();
	debugfs_remove_recursive(criu_root);
	pr_info("criu_kernel: unloaded\n");
}

module_init(criu_init);
module_exit(criu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A1 read-only task and VMA probe for Linux 5.10.29 arm64");
