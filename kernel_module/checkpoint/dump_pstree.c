/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "../../include/criu_snapshot.h"
#include "criu_kernel.h"
#include "dump_pstree.h"
#include "snapshot_writer.h"

struct pstree_order {
	u32 pid;
	unsigned int index;
};

static int pstree_compare(const void *left, const void *right)
{
	const struct pstree_order *a = left;
	const struct pstree_order *b = right;

	return (a->pid > b->pid) - (a->pid < b->pid);
}

int criu_dump_pstree(struct criu_freeze_ctx *ctx,
		     struct criu_snapshot_writer *writer)
{
	struct pstree_order *order;
	struct criu_snapshot_pstree_record record;
	unsigned int count, i;
	int ret;

	if (!ctx || !writer || criu_freeze_process_count(ctx, &count) || !count)
		return -EINVAL;
	order = kcalloc(count, sizeof(*order), GFP_KERNEL);
	if (!order)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		struct criu_freeze_process_view view;

		ret = criu_freeze_process_get(ctx, i, &view);
		if (ret) {
			kfree(order);
			return ret;
		}
		order[i].pid = view.pid;
		order[i].index = i;
	}
	sort(order, count, sizeof(*order), pstree_compare, NULL);
	for (i = 0; i < count; i++) {
		struct criu_freeze_process_view view;
		u32 flags = 0;

		ret = criu_freeze_process_get(ctx, order[i].index, &view);
		if (ret)
			break;
		if (view.root)
			flags |= CRIU_SNAPSHOT_PSTREE_F_ROOT;
		if (view.external_parent)
			flags |= CRIU_SNAPSHOT_PSTREE_F_EXTERNAL_PARENT;
		if (view.session_leader)
			flags |= CRIU_SNAPSHOT_PSTREE_F_SESSION_LEADER;
		if (view.process_group_leader)
			flags |= CRIU_SNAPSHOT_PSTREE_F_PGRP_LEADER;
		memset(&record, 0, sizeof(record));
		record.version = cpu_to_le32(CRIU_SNAPSHOT_PSTREE_VERSION);
		record.flags = cpu_to_le32(flags);
		record.pid = cpu_to_le32(view.pid);
		record.tgid = cpu_to_le32(view.tgid);
		record.ppid = cpu_to_le32(view.ppid);
		record.pgid = cpu_to_le32(view.pgid);
		record.sid = cpu_to_le32(view.sid);
		record.born_sid = cpu_to_le32((u32)view.born_sid);
		record.leader_pid = cpu_to_le32(view.pid);
		record.thread_count = cpu_to_le32(view.task_count);
		record.namespace_scope =
			cpu_to_le32(CRIU_SNAPSHOT_PSTREE_NAMESPACE_CURRENT);
		ret = criu_snapshot_writer_record(
			writer, CRIU_SNAPSHOT_REC_PSTREE, 0,
			&record, sizeof(record));
		if (ret)
			break;
	}
	kfree(order);
	return ret;
}
