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
#include <linux/magic.h>
#include <linux/skbuff.h>
#include <net/af_unix.h>
#include <net/sock.h>

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

/* An A5 IPC object must be closed over the frozen fd table. Extra references
 * may belong to an external task or in-flight operation; reject conservatively.
 * Each local descriptor has its original and our pinned reference. */
static int validate_ipc_scope(struct file **snapshot, unsigned int count)
{
	unsigned int i, j, refs, readers, writers;

	for (i = 0; i < count; i++) {
		struct file *file = snapshot[i];
		struct pipe_inode_info *pipe;

		if (!file || (!S_ISFIFO(file_inode(file)->i_mode) &&
			      !S_ISSOCK(file_inode(file)->i_mode)))
			continue;
		refs = 0;
		for (j = 0; j < count; j++)
			if (snapshot[j] == file)
				refs++;
		if (file_count(file) != 2UL * refs)
			return -EOPNOTSUPP;
		if (!S_ISFIFO(file_inode(file)->i_mode))
			continue;
		if (file_inode(file)->i_sb->s_magic != PIPEFS_MAGIC)
			return -EOPNOTSUPP;
		pipe = file->private_data;
		if (!pipe)
			return -EOPNOTSUPP;
		readers = writers = 0;
		for (j = 0; j < count; j++) {
			unsigned int k;
			struct file *other = snapshot[j];
			if (!other || file_inode(other) != file_inode(file))
				continue;
			for (k = 0; k < j; k++)
				if (snapshot[k] == other)
					break;
			if (k != j)
				continue;
			readers += !!(other->f_mode & FMODE_READ);
			writers += !!(other->f_mode & FMODE_WRITE);
		}
		pipe_lock(pipe);
		j = readers == pipe->readers && writers == pipe->writers;
		pipe_unlock(pipe);
		if (!j)
			return -EOPNOTSUPP;
	}
	return 0;
}

static int validate_files_scope(struct task_struct *task, struct files_struct *files)
{
	struct task_struct *thread;
	unsigned int owners = 0;
	int ret = 0;

	/* 5.10 for_each_thread includes the leader. A single files image cannot
	 * represent unshared per-thread tables or an unfrozen CLONE_FILES owner. */
	rcu_read_lock();
	for_each_thread(task, thread) {
		task_lock(thread);
		if (thread->files != files)
			ret = -EOPNOTSUPP;
		task_unlock(thread);
		owners++;
	}
	rcu_read_unlock();
	if (atomic_read(&files->count) != owners + 1)
		ret = -EOPNOTSUPP;
	return ret;
}

static int validate_files_scope_count(struct files_struct *files,
				      unsigned int frozen_owners)
{
	if (!files || !frozen_owners)
		return -EINVAL;
	if (atomic_read(&files->count) != frozen_owners + 1)
		return -EOPNOTSUPP;
	return 0;
}

static int walk_fds_prepared(struct task_struct *task, criu_fd_fn prepare,
			    criu_fd_fn fn, void *arg,
			    unsigned int frozen_owners)
{
	struct files_struct *files;
	struct fdtable *fdt;
	struct file **snapshot;
	unsigned int *fd_flags;
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
	if (prepare) {
		ret = frozen_owners ?
			validate_files_scope_count(files, frozen_owners) :
			validate_files_scope(task, files);
		if (ret)
			goto out_files;
	}
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	count = fdt->max_fds;
	spin_unlock(&files->file_lock);
	snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
	fd_flags = kcalloc(count, sizeof(*fd_flags), GFP_KERNEL);
	if (!snapshot || !fd_flags) {
		kfree(snapshot);
		kfree(fd_flags);
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
			fd_flags[i] = close_on_exec(i, fdt) ? CRIU_FD_FLAG_CLOEXEC : 0;
		}
	}
	spin_unlock(&files->file_lock);
	if (prepare) {
		ret = validate_ipc_scope(snapshot, count);
		if (ret)
			goto release;
	}
	for (i = 0; i < count; i++) {
		if (prepare && snapshot[i]) {
			ret = prepare(i, snapshot[i], fd_flags[i], arg);
			if (ret)
				goto release;
		}
	}
	for (i = 0; i < count; i++) {
		if (!snapshot[i])
			continue;
		ret = fn(i, snapshot[i], fd_flags[i], arg);
		if (ret)
			break;
	}
release:
	/* Keep all objects pinned across discovery and serialization; release
	 * each reference exactly once, including callback error paths. */
	for (i = 0; i < count; i++) {
		if (snapshot[i])
			fput(snapshot[i]);
	}
	kfree(snapshot);
	kfree(fd_flags);
out_files:
	if (WARN_ON_ONCE(atomic_dec_and_test(&files->count)))
		atomic_inc(&files->count);
	return ret;
}

int criu_walk_fds(struct task_struct *task, criu_fd_fn fn, void *arg)
{
	return walk_fds_prepared(task, NULL, fn, arg, 0);
}

struct dump_fd_ctx {
	struct criu_snapshot_writer *writer;
	struct criu_objmap *objects;
	struct criu_objmap *emitted;
	struct criu_objmap *pipes;
};

static int dump_pipe_state(unsigned int fd, struct file *file,
				struct dump_fd_ctx *ctx, u64 object_id)
{
	struct pipe_inode_info *pipe = file->private_data;
	struct criu_snapshot_pipe_endpoint_record endpoint;
	struct criu_snapshot_pipe_data_record *data;
	unsigned int i, bytes = 0;
	char *payload;
	bool read_end = !!(file->f_mode & FMODE_READ);
	bool is_new;
	int ret;

	if (!pipe || !pipe->bufs || !pipe->ring_size ||
		file_inode(file)->i_sb->s_magic != PIPEFS_MAGIC ||
		(file->f_flags & O_DIRECT))
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
	if (!criu_objmap_get(ctx->pipes, pipe, &is_new))
		return -ENOMEM;
	if (!is_new)
		return 0;
	pipe_lock(pipe);
	if (pipe->head - pipe->tail > pipe->ring_size) {
		pipe_unlock(pipe);
		return -EIO;
	}
	for (i = pipe->tail; i != pipe->head; i++) {
		struct pipe_buffer *buf = &pipe->bufs[i & (pipe->ring_size - 1)];
		if (!buf->page || !buf->ops || buf->ops->confirm ||
			(buf->flags & PIPE_BUF_FLAG_PACKET) || buf->offset > PAGE_SIZE ||
			buf->len > PAGE_SIZE - buf->offset)
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
	data->capacity = (u64)pipe->max_usage * PAGE_SIZE;
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
		sk->sk_state != TCP_ESTABLISHED ||
		test_bit(SOCK_PASSCRED, &sk->sk_socket->flags) ||
		test_bit(SOCK_PASSSEC, &sk->sk_socket->flags))
		return -EOPNOTSUPP;
	peer = unix_peer_get(sk);
	if (!peer)
		return -EOPNOTSUPP;
	memset(&rec, 0, sizeof(rec));
	rec.version = CRIU_SNAPSHOT_UNIX_SOCKET_VERSION;
	rec.object_id = object_id;
	rec.peer_object_id = criu_objmap_find(ctx->objects, peer);
	if (!rec.peer_object_id || unix_sk(peer)->peer != sk || unix_sk(sk)->addr ||
		unix_sk(peer)->addr) {
		ret = -EOPNOTSUPP;
		goto out_peer;
	}
	rec.family = AF_UNIX;
	rec.socket_type = SOCK_STREAM;
	rec.state = sk->sk_state;
	rec.shutdown = sk->sk_shutdown;
	rec.options = (u64)(u32)sk->sk_sndbuf | ((u64)(u32)sk->sk_rcvbuf << 32);
	ret = criu_snapshot_writer_record(ctx->writer,
			CRIU_SNAPSHOT_REC_UNIX_SOCKET, 0, &rec, sizeof(rec));
	if (ret)
		goto out_peer;
	spin_lock_bh(&sk->sk_receive_queue.lock);
	skb_queue_walk(&sk->sk_receive_queue, skb) {
		if (UNIXCB(skb).fp || UNIXCB(skb).pid ||
		    UNIXCB(skb).consumed > skb->len ||
		    skb->len - UNIXCB(skb).consumed > UINT_MAX - bytes) {
			spin_unlock_bh(&sk->sk_receive_queue.lock);
			ret = -EOPNOTSUPP;
			goto out_peer;
		}
		/* 5.10 stream reads advance consumed, not skb->data/len. */
		bytes += skb->len - UNIXCB(skb).consumed;
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
			unsigned int consumed = UNIXCB(skb).consumed;
			unsigned int remaining;

			if (consumed > skb->len || UNIXCB(skb).fp || UNIXCB(skb).pid ||
			    (remaining = skb->len - consumed) > queue->data_len - bytes ||
			    skb_copy_bits(skb, consumed,
					  payload + sizeof(*queue) + bytes, remaining)) {
				spin_unlock_bh(&sk->sk_receive_queue.lock);
				kfree(payload);
				ret = -EIO;
				goto out_peer;
			}
			bytes += remaining;
		}
		spin_unlock_bh(&sk->sk_receive_queue.lock);
		if (bytes != queue->data_len) {
			kfree(payload);
			ret = -EAGAIN;
			goto out_peer;
		}
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

static int dump_one_fd(unsigned int fd, struct file *file,
		       unsigned int fd_flags, void *arg)
{
	struct dump_fd_ctx *ctx = arg;
	struct criu_snapshot_fd_record rec;
	struct inode *inode = file_inode(file);
	bool is_new;
	u32 type;
	int ret;

	/* fown-driven signal delivery belongs to A6; never silently reset it. */
	read_lock(&file->f_owner.lock);
	ret = file->f_owner.pid ? -EOPNOTSUPP : 0;
	read_unlock(&file->f_owner.lock);
	if (ret || (file->f_flags & FASYNC))
		return -EOPNOTSUPP;
	if (S_ISREG(inode->i_mode))
		type = CRIU_FD_TYPE_REG;
	else if (S_ISFIFO(inode->i_mode))
		type = CRIU_FD_TYPE_PIPE;
	else if (S_ISSOCK(inode->i_mode))
		type = CRIU_FD_TYPE_UNIX;
	else
		return -EOPNOTSUPP;
	if (type == CRIU_FD_TYPE_REG) {
		struct file_lock_context *locks = smp_load_acquire(&inode->i_flctx);
		bool locked = false;

		if (locks) {
			spin_lock(&locks->flc_lock);
			locked = !list_empty(&locks->flc_flock) ||
				 !list_empty(&locks->flc_posix) ||
				 !list_empty(&locks->flc_lease);
			spin_unlock(&locks->flc_lock);
		}
		if (locked)
			return -EOPNOTSUPP;
	}
	if (type != CRIU_FD_TYPE_REG && type != CRIU_FD_TYPE_PIPE &&
		type != CRIU_FD_TYPE_UNIX)
		return -EOPNOTSUPP;
	memset(&rec, 0, sizeof(rec));
	rec.fd = fd;
	rec.object_flags = fd_flags;
	rec.mode = inode->i_mode;
	rec.flags = file->f_flags & ~(O_CREAT | O_EXCL | O_TRUNC);
	rec.pos = file->f_pos;
	rec.dev = inode->i_sb->s_dev;
	rec.ino = inode->i_ino;
	rec.size = i_size_read(inode);
	rec.type = type;
	rec.object_id = criu_objmap_find(ctx->objects,
			type == CRIU_FD_TYPE_UNIX ? (void *)unix_get_socket(file) : (void *)file);
	if (!rec.object_id)
		return -EIO;
	if (!criu_objmap_get(ctx->emitted, file, &is_new))
		return -ENOMEM;
	if (type == CRIU_FD_TYPE_PIPE && is_new) {
		ret = dump_pipe_state(fd, file, ctx, rec.object_id);
		if (ret)
			return ret;
	} else if (type == CRIU_FD_TYPE_UNIX && is_new) {
		ret = dump_unix_state(file, ctx, rec.object_id);
		if (ret)
			return ret;
	} else if (type == CRIU_FD_TYPE_REG) {
		ret = path_text(&file->f_path, rec.path, sizeof(rec.path));
		if (ret || strstr(rec.path, " (deleted)"))
			return ret ? ret : -EOPNOTSUPP;
	}
	ret = criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_FD,
						 0, &rec, sizeof(rec));
	return ret;
}

static int map_one_fd(unsigned int fd, struct file *file,
		      unsigned int fd_flags, void *arg)
{
	struct dump_fd_ctx *ctx = arg;
	void *key = S_ISSOCK(file_inode(file)->i_mode) ?
		(void *)unix_get_socket(file) : (void *)file;

	if (!key)
		return -EOPNOTSUPP;
	return criu_objmap_get(ctx->objects, key, NULL) ? 0 : -ENOMEM;
}

static int criu_dump_files_common(struct task_struct *task,
			struct criu_snapshot_writer *writer,
			unsigned int frozen_owners,
			struct criu_dump_shared_ctx *shared_ctx)
{
	struct criu_fs_record fs;
	struct path cwd, root;
	struct dump_fd_ctx fd_ctx;
	int ret = 0;

	if (!task || !writer || !shared_ctx)
		return -EINVAL;
	fd_ctx.writer = writer;
	fd_ctx.objects = shared_ctx->file_objects;
	fd_ctx.emitted = shared_ctx->emitted_file_objects;
	fd_ctx.pipes = shared_ctx->pipes;
	ret = walk_fds_prepared(task, map_one_fd, dump_one_fd, &fd_ctx,
				frozen_owners);
	if (ret)
		return ret;

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
	return ret;
}

int criu_dump_files(struct task_struct *task,
			struct criu_snapshot_writer *writer)
{
	struct criu_dump_shared_ctx shared_ctx;
	int ret;

	ret = criu_dump_shared_ctx_init(&shared_ctx);
	if (ret)
		return ret;
	ret = criu_dump_files_common(task, writer, 0, &shared_ctx);
	criu_dump_shared_ctx_destroy(&shared_ctx);
	return ret;
}

int criu_dump_files_process(struct criu_freeze_ctx *ctx,
			    unsigned int process_index,
			    const struct criu_freeze_process_view *view,
			    struct criu_snapshot_writer *writer,
			    struct criu_dump_shared_ctx *shared_ctx)
{
	unsigned int task_count;
	unsigned int process_count;
	unsigned int i;
	unsigned int frozen_owners = 0;
	struct files_struct *target_files;
	int ret;

	if (!ctx || !view || !writer || !shared_ctx)
		return -EINVAL;
	ret = criu_freeze_process_task_count(ctx, process_index, &task_count);
	if (ret)
		return ret;
	for (i = 0; i < task_count; i++) {
		struct criu_freeze_task_view task_view;

		ret = criu_freeze_process_task_get(ctx, process_index, i,
						   &task_view);
		if (ret)
			return ret;
		task_lock(task_view.task);
		if (task_view.task->files != view->leader->files)
			ret = -EOPNOTSUPP;
		task_unlock(task_view.task);
		if (ret)
			return ret;
	}
	task_lock(view->leader);
	target_files = view->leader->files;
	task_unlock(view->leader);
	if (!target_files)
		return -ESRCH;
	ret = criu_freeze_process_count(ctx, &process_count);
	if (ret)
		return ret;
	for (i = 0; i < process_count; i++) {
		unsigned int owner_tasks;
		struct criu_freeze_process_view other;
		struct files_struct *other_files;

		ret = criu_freeze_process_get(ctx, i, &other);
		if (ret)
			return ret;
		task_lock(other.leader);
		other_files = other.leader->files;
		task_unlock(other.leader);
		if (other_files != target_files)
			continue;
		ret = criu_freeze_process_task_count(ctx, i, &owner_tasks);
		if (ret)
			return ret;
		if (frozen_owners > UINT_MAX - owner_tasks)
			return -EOVERFLOW;
		frozen_owners += owner_tasks;
	}
	if (!frozen_owners)
		return -ESRCH;
	return criu_dump_files_common(view->leader, writer, frozen_owners,
					shared_ctx);
}
