// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/string.h>

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
