/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_SHMEM_H
#define CRIU_DUMP_SHMEM_H

#include <linux/types.h>

#include "dump_shared.h"
#include "snapshot_writer.h"

int criu_shmem_register(struct criu_dump_shared_ctx *ctx,
			struct inode *inode, u64 size,
			u32 *shmid, bool *is_new);
int criu_shmem_dump_content(struct criu_dump_shared_ctx *ctx,
			    struct criu_snapshot_writer *writer,
			    struct inode *inode, u32 shmid, u64 size);

#endif
