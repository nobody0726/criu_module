/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_SHARED_H
#define CRIU_DUMP_SHARED_H

#include "../core/objmap.h"
#include "../include/criu_kernel.h"
#include "snapshot_writer.h"

struct criu_dump_shared_ctx {
	struct criu_objmap *mm_ids;
	struct criu_objmap *files_ids;
	struct criu_objmap *fs_ids;
	struct criu_objmap *sighand_ids;
	struct criu_objmap *file_objects;
	struct criu_objmap *emitted_file_objects;
	struct criu_objmap *pipes;
	struct criu_objmap *unix_sockets;
	struct criu_objmap *shmem_ids;
	struct criu_objmap *emitted_shmem;
};

int criu_dump_shared_ctx_init(struct criu_dump_shared_ctx *ctx);
void criu_dump_shared_ctx_destroy(struct criu_dump_shared_ctx *ctx);
int criu_dump_task_ids_process(struct criu_dump_shared_ctx *ctx,
			       const struct criu_freeze_process_view *view,
			       struct criu_snapshot_writer *writer);

#endif
