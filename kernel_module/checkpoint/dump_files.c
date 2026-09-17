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
#include <linux/pipe_fs_i.h>
#include <linux/highmem.h>
#include <linux/skbuff.h>
#include <net/af_unix.h>
#include <net/sock.h>

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

static int dump_pipe_state(unsigned int fd, struct file *file,
				struct dump_fd_ctx *ctx, u64 object_id)
{
	struct pipe_inode_info *pipe = file->private_data;
	struct criu_snapshot_pipe_endpoint_record endpoint;
	struct criu_snapshot_pipe_data_record *data;
	unsigned int i, used, bytes = 0;
	char *payload;
	bool read_end = !!(file->f_mode & FMODE_READ);
	int ret;

	if (!pipe || !pipe->bufs || !pipe->ring_size)
		return -EOPNOTSUPP;
	memset(&endpoint, 0, sizeof(endpoint));
	endpoint.version = CRIU_SNAPSHOT_PIPE_ENDPOINT_VERSION;
	endpoint.object_id = object_id;
	endpoint.pipe_id = file_inode(file)->i_ino;
	endpoint.direction = read_end ? CRIU_PIPE_DIRECTION_READ : CRIU_PIPE_DIRECTION_WRITE;
	if (!pipe->writers)
		endpoint.flags |= CRIU_PIPE_FLAG_WRITE_CLOSED;
	ret = criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_PIPE_ENDPOINT,
						0, &endpoint, sizeof(endpoint));
	if (ret || !read_end)
		return ret;
	pipe_lock(pipe);
	used = pipe->head - pipe->tail;
	for (i = pipe->tail; i != pipe->head; i++) {
		struct pipe_buffer *buf = &pipe->bufs[i & (pipe->ring_size - 1)];
		if (!buf->page || !buf->ops || buf->ops->confirm || buf->len > PAGE_SIZE)
			{ pipe_unlock(pipe); return -EOPNOTSUPP; }
		if (bytes > UINT_MAX - buf->len) { pipe_unlock(pipe); return -EOVERFLOW; }
		bytes += buf->len;
	}
	payload = kmalloc(sizeof(*data) + bytes, GFP_KERNEL);
	if (!payload) { pipe_unlock(pipe); return -ENOMEM; }
	data = (struct criu_snapshot_pipe_data_record *)payload;
	memset(data, 0, sizeof(*data));
	data->version = CRIU_SNAPSHOT_PIPE_DATA_VERSION;
	data->pipe_id = endpoint.pipe_id;
	data->capacity = (u64)pipe->ring_size * PAGE_SIZE;
	data->data_len = bytes;
	bytes = 0;
	for (i = pipe->tail; i != pipe->head; i++) {
		struct pipe_buffer *buf = &pipe->bufs[i & (pipe->ring_size - 1)];
		void *mapped = kmap_atomic(buf->page);
		memcpy(payload + sizeof(*data) + bytes, mapped + buf->offset, buf->len);
		kunmap_atomic(mapped);
		bytes += buf->len;
	}
	pipe_unlock(pipe);
	ret = criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_PIPE_DATA,
						0, payload, sizeof(*data) + data->data_len);
	kfree(payload);
	return ret;
}

static int dump_unix_state(struct file *file, struct dump_fd_ctx *ctx,
				 u64 object_id)
{
	struct sock *sk, *peer;
	struct criu_snapshot_unix_socket_record rec;
	struct sk_buff *skb;
	unsigned int bytes = 0;
	char *payload;
	int ret;

	sk = unix_get_socket(file);
	if (!sk || sk->sk_family != AF_UNIX || sk->sk_type != SOCK_STREAM ||
		sk->sk_state != TCP_ESTABLISHED)
		return -EOPNOTSUPP;
	peer = unix_peer_get(sk);
	if (!peer)
		return -EOPNOTSUPP;
	memset(&rec, 0, sizeof(rec));
	rec.version = CRIU_SNAPSHOT_UNIX_SOCKET_VERSION;
	rec.object_id = object_id;
	rec.peer_object_id = (u64)peer->sk_socket ?
		(u64)file_inode(peer->sk_socket->file)->i_ino : 0;
	rec.family = AF_UNIX;
	rec.socket_type = SOCK_STREAM;
	rec.state = sk->sk_state;
	ret = criu_snapshot_writer_record(ctx->writer,
			CRIU_SNAPSHOT_REC_UNIX_SOCKET, 0, &rec, sizeof(rec));
	if (ret)
		goto out_peer;
	spin_lock_bh(&sk->sk_receive_queue.lock);
	skb_queue_walk(&sk->sk_receive_queue, skb) {
		if (UNIXCB(skb).fp || skb->len > UINT_MAX - bytes) {
			spin_unlock_bh(&sk->sk_receive_queue.lock);
			ret = -EOPNOTSUPP;
			goto out_peer;
		}
		bytes += skb->len;
	}
	spin_unlock_bh(&sk->sk_receive_queue.lock);
	payload = kmalloc(sizeof(struct criu_snapshot_socket_queue_record) + bytes,
			GFP_KERNEL);
	if (!payload) { ret = -ENOMEM; goto out_peer; }
	{
		struct criu_snapshot_socket_queue_record *queue =
			(struct criu_snapshot_socket_queue_record *)payload;
		memset(queue, 0, sizeof(*queue));
		queue->version = CRIU_SNAPSHOT_SOCKET_QUEUE_VERSION;
		queue->object_id = object_id;
		queue->data_len = bytes;
		spin_lock_bh(&sk->sk_receive_queue.lock);
		bytes = 0;
		skb_queue_walk(&sk->sk_receive_queue, skb) {
			if (skb_copy_bits(skb, 0, payload + sizeof(*queue) + bytes,
					skb->len)) {
				spin_unlock_bh(&sk->sk_receive_queue.lock);
				kfree(payload);
				ret = -EIO;
				goto out_peer;
			}
			bytes += skb->len;
		}
		spin_unlock_bh(&sk->sk_receive_queue.lock);
	}
	ret = criu_snapshot_writer_record(ctx->writer,
			CRIU_SNAPSHOT_REC_SOCKET_QUEUE, 0, payload,
			sizeof(struct criu_snapshot_socket_queue_record) +
			((struct criu_snapshot_socket_queue_record *)payload)->data_len);
	kfree(payload);
out_peer:
	sock_put(peer);
	return ret;
}

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
	if (type != CRIU_FD_TYPE_REG && type != CRIU_FD_TYPE_PIPE &&
		type != CRIU_FD_TYPE_UNIX)
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
	rec.object_id = criu_objmap_get(ctx->objects, file, &is_new);
	if (!rec.object_id)
		return -ENOMEM;
	if (type == CRIU_FD_TYPE_PIPE) {
		ret = dump_pipe_state(fd, file, ctx, rec.object_id);
		if (ret)
			return ret;
	} else if (type == CRIU_FD_TYPE_UNIX) {
		ret = dump_unix_state(file, ctx, rec.object_id);
		if (ret)
			return ret;
	} else {
		ret = path_text(&file->f_path, rec.path, sizeof(rec.path));
		if (ret || strstr(rec.path, " (deleted)"))
			return ret ? ret : -EOPNOTSUPP;
	}
	ret = criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_FD,
						 0, &rec, sizeof(rec));
	return ret;
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
