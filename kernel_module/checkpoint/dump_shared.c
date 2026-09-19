// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/string.h>

#include "../../include/criu_snapshot.h"
#include "dump_shared.h"

static int init_map(struct criu_objmap **map)
{
	*map = criu_objmap_new();
	return *map ? 0 : -ENOMEM;
}

int criu_dump_shared_ctx_init(struct criu_dump_shared_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ret = init_map(&ctx->mm_ids);
	if (ret)
		goto err;
	ret = init_map(&ctx->files_ids);
	if (ret)
		goto err;
	ret = init_map(&ctx->fs_ids);
	if (ret)
		goto err;
	ret = init_map(&ctx->sighand_ids);
	if (ret)
		goto err;
	ret = init_map(&ctx->file_objects);
	if (ret)
		goto err;
	ret = init_map(&ctx->emitted_file_objects);
	if (ret)
		goto err;
	ret = init_map(&ctx->pipes);
	if (ret)
		goto err;
	ret = init_map(&ctx->unix_sockets);
	if (ret)
		goto err;
	ret = init_map(&ctx->shmem_ids);
	if (ret)
		goto err;
	ret = init_map(&ctx->emitted_shmem);
	if (ret)
		goto err;
	return 0;
err:
	criu_dump_shared_ctx_destroy(ctx);
	return ret;
}

void criu_dump_shared_ctx_destroy(struct criu_dump_shared_ctx *ctx)
{
	if (!ctx)
		return;
	criu_objmap_free(ctx->emitted_shmem);
	criu_objmap_free(ctx->shmem_ids);
	criu_objmap_free(ctx->unix_sockets);
	criu_objmap_free(ctx->pipes);
	criu_objmap_free(ctx->emitted_file_objects);
	criu_objmap_free(ctx->file_objects);
	criu_objmap_free(ctx->sighand_ids);
	criu_objmap_free(ctx->fs_ids);
	criu_objmap_free(ctx->files_ids);
	criu_objmap_free(ctx->mm_ids);
	memset(ctx, 0, sizeof(*ctx));
}

int criu_dump_task_ids_process(struct criu_dump_shared_ctx *ctx,
			       const struct criu_freeze_process_view *view,
			       struct criu_snapshot_writer *writer)
{
	struct criu_snapshot_task_ids_record rec;
	struct mm_struct *mm;
	struct files_struct *files;
	struct fs_struct *fs;
	struct sighand_struct *sighand;
	u32 vm_id, files_id, fs_id, sighand_id;

	if (!ctx || !view || !view->leader || !writer)
		return -EINVAL;
	mm = get_task_mm(view->leader);
	if (!mm)
		return -ESRCH;
	task_lock(view->leader);
	files = view->leader->files;
	fs = view->leader->fs;
	sighand = view->leader->sighand;
	task_unlock(view->leader);
	if (!files || !fs || !sighand) {
		mmput(mm);
		return -ESRCH;
	}
	vm_id = criu_objmap_get(ctx->mm_ids, mm, NULL);
	files_id = criu_objmap_get(ctx->files_ids, files, NULL);
	fs_id = criu_objmap_get(ctx->fs_ids, fs, NULL);
	sighand_id = criu_objmap_get(ctx->sighand_ids, sighand, NULL);
	mmput(mm);
	if (!vm_id || !files_id || !fs_id || !sighand_id)
		return -ENOMEM;
	memset(&rec, 0, sizeof(rec));
	rec.version = cpu_to_le32(CRIU_SNAPSHOT_TASK_IDS_VERSION);
	rec.pid = cpu_to_le32(view->pid);
	rec.vm_id = cpu_to_le32(vm_id);
	rec.files_id = cpu_to_le32(files_id);
	rec.fs_id = cpu_to_le32(fs_id);
	rec.sighand_id = cpu_to_le32(sighand_id);
	return criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_TASK_IDS,
					   0, &rec, sizeof(rec));
}
