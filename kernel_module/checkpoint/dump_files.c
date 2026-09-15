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
#include <linux/socket.h>

#include "../core/objmap.h"
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

int criu_walk_fds(struct task_struct *task, criu_fd_fn fn, void *arg)
{
	struct files_struct *files;
	struct fdtable *fdt;
	struct file **snapshot;
	unsigned int i, count;
	int ret = 0;

	if (!task || !fn)
		return -EINVAL;
	task_lock(task);
	files = task->files;
	if (files)
		atomic_inc(&files->count);
	task_unlock(task);
	if (!files)
		return -ESRCH;
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	count = fdt->max_fds;
	spin_unlock(&files->file_lock);
	snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto out_files;
	}
	/* A2 leaves the target quiescent. Pin references while holding file_lock,
	 * then invoke callbacks after releasing it so path/object I/O can sleep. */
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	for (i = 0; i < count; i++) {
		if (fdt->fd[i]) {
			get_file(fdt->fd[i]);
			snapshot[i] = fdt->fd[i];
		}
	}
	spin_unlock(&files->file_lock);
	for (i = 0; i < count; i++) {
		if (!snapshot[i])
			continue;
		ret = fn(i, snapshot[i], arg);
		fput(snapshot[i]);
		if (ret)
			break;
	}
	while (i < count) {
		if (snapshot[i])
			fput(snapshot[i]);
		i++;
	}
	kfree(snapshot);
out_files:
	if (WARN_ON_ONCE(atomic_dec_and_test(&files->count)))
		atomic_inc(&files->count);
	return ret;
}

struct dump_fd_ctx {
	struct criu_snapshot_writer *writer;
	struct criu_objmap *objects;
};

static int dump_one_fd(unsigned int fd, struct file *file, void *arg)
{
	struct dump_fd_ctx *ctx = arg;
	struct criu_snapshot_fd_record rec;
	struct inode *inode = file_inode(file);
	bool is_new;
	u32 type;
	int ret;

	if (S_ISREG(inode->i_mode))
		type = CRIU_FD_TYPE_REG;
	else if (S_ISFIFO(inode->i_mode))
		type = CRIU_FD_TYPE_PIPE;
	else if (S_ISSOCK(inode->i_mode))
		type = CRIU_FD_TYPE_UNIX;
	else
		return -EOPNOTSUPP;
	if (type != CRIU_FD_TYPE_REG)
		return -EOPNOTSUPP;
	memset(&rec, 0, sizeof(rec));
	rec.fd = fd;
	rec.mode = inode->i_mode;
	rec.flags = file->f_flags & ~(O_CREAT | O_EXCL | O_TRUNC);
	rec.pos = file->f_pos;
	rec.dev = inode->i_sb->s_dev;
	rec.ino = inode->i_ino;
	rec.size = i_size_read(inode);
	rec.type = type;
	ret = path_text(&file->f_path, rec.path, sizeof(rec.path));
	if (ret)
		return ret;
	if (strstr(rec.path, " (deleted)"))
		return -EOPNOTSUPP;
	rec.object_id = criu_objmap_get(ctx->objects, file, &is_new);
	if (!rec.object_id)
		return -ENOMEM;
	return criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_FD,
					 0, &rec, sizeof(rec));
}

int criu_dump_files(struct task_struct *task,
			struct criu_snapshot_writer *writer)
{
	struct criu_fs_record fs;
	struct path cwd, root;
	struct criu_objmap *objects;
	struct dump_fd_ctx fd_ctx;
	int ret = 0;

	if (!task || !writer)
		return -EINVAL;
	objects = criu_objmap_new();
	if (!objects)
		return -ENOMEM;
	fd_ctx.writer = writer;
	fd_ctx.objects = objects;
	ret = criu_walk_fds(task, dump_one_fd, &fd_ctx);
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
	criu_objmap_free(objects);
	return ret;
}
