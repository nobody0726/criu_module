/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_PSTREE_H
#define CRIU_DUMP_PSTREE_H

struct criu_freeze_ctx;
struct criu_snapshot_writer;

int criu_dump_pstree(struct criu_freeze_ctx *ctx,
		     struct criu_snapshot_writer *writer);

#endif
