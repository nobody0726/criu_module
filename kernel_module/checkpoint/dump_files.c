/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/path.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "dump_files.h"

#define CRIU_SNAPSHOT_UNSUPPORTED (-EOPNOTSUPP)

static int path_text(const struct path *path, char *out, size_t size)
{
	char *buf, *name;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	name = d_path(path, buf, PAGE_SIZE);
	if (IS_ERR(name)) {
		kfree(buf);
		return PTR_ERR(name);
	}
	if (strscpy(out, name, size) < 0) {
		kfree(buf);
		return -ENAMETOOLONG;
	}
	kfree(buf);
	return 0;
}

int criu_dump_files(struct task_struct *task,
			struct criu_snapshot_writer *writer)
{
	struct files_struct *files;
	struct fdtable *fdt;
	struct criu_fs_record fs;
	struct path cwd, root;
	unsigned int fd, max_fds;
	int ret = 0;

	if (!task || !writer)
		return -EINVAL;
	/*
	 * The target is held by the freeze context and all its threads are
	 * quiescent, so task->files cannot be replaced or freed during this
	 * scan.  get_files_struct()/put_files_struct() are intentionally not
	 * used: Linux 5.10.29 keeps those helpers internal to fs/file.c.
	 */
	task_lock(task);
	files = task->files;
	if (files)
		atomic_inc(&files->count);
	task_unlock(task);
	if (!files)
		return -ESRCH;
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	max_fds = fdt->max_fds;
	spin_unlock(&files->file_lock);

	for (fd = 0; fd < max_fds; fd++) {
		struct file *file;
		struct criu_fd_record rec;

		spin_lock(&files->file_lock);
		fdt = files_fdtable(files);
		file = fdt->fd[fd];
		if (file)
			get_file(file);
		spin_unlock(&files->file_lock);
		if (!file)
			continue;
		if (fd > 2 || !S_ISREG(file_inode(file)->i_mode)) {
			ret = -EOPNOTSUPP;
			fput(file);
			break;
		}
		/* A3 only permits the conventional stdin/stdout/stderr set. */
		if (fd == 0 || fd == 1 || fd == 2) {
			/* fd 0/1/2 are the only accepted descriptors. */
		}
		memset(&rec, 0, sizeof(rec));
		rec.fd = fd;
		rec.mode = file_inode(file)->i_mode;
		rec.flags = file->f_flags;
		rec.pos = file->f_pos;
		rec.dev = file_inode(file)->i_sb->s_dev;
		rec.ino = file_inode(file)->i_ino;
		rec.size = i_size_read(file_inode(file));
		ret = path_text(&file->f_path, rec.path, sizeof(rec.path));
		if (ret) {
			fput(file);
			break;
		}
		ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_FD,
						  0, &rec, sizeof(rec));
		fput(file);
		if (ret)
			break;
	}
	if (ret)
		goto out;

	get_fs_pwd(task->fs, &cwd);
	get_fs_root(task->fs, &root);
	memset(&fs, 0, sizeof(fs));
	ret = path_text(&cwd, fs.cwd, sizeof(fs.cwd));
	if (!ret)
		ret = path_text(&root, fs.root, sizeof(fs.root));
	path_put(&cwd);
	path_put(&root);
	if (!ret)
		ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_FS,
						  0, &fs, sizeof(fs));
out:
	/* The frozen target still owns its files_struct, so this cannot reach zero. */
	if (WARN_ON_ONCE(atomic_dec_and_test(&files->count)))
		atomic_inc(&files->count);
	return ret;
}
