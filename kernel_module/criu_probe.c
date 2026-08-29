#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>

static struct dentry *criu_probe_root;

static int criu_probe_status_show(struct seq_file *m, void *unused)
{
	seq_puts(m, "criu_probe:ok\n");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(criu_probe_status);

static int __init criu_probe_init(void)
{
	criu_probe_root = debugfs_create_dir("criu_probe", NULL);
	if (!criu_probe_root)
		return -ENOMEM;

	if (!debugfs_create_file("status", 0444, criu_probe_root, NULL,
				&criu_probe_status_fops)) {
		debugfs_remove_recursive(criu_probe_root);
		return -ENOMEM;
	}

	pr_info("criu_probe: loaded\n");
	return 0;
}

static void __exit criu_probe_exit(void)
{
	debugfs_remove_recursive(criu_probe_root);
	pr_info("criu_probe: unloaded\n");
}

module_init(criu_probe_init);
module_exit(criu_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal CRIU kernel module environment probe");
