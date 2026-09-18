/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_SIGNALS_H
#define CRIU_DUMP_SIGNALS_H

#include <linux/types.h>

#include "../../include/criu_snapshot.h"

struct criu_freeze_ctx;

struct criu_signal_queue_chunk {
	u32 scope;
	u32 owner_tid;
	u32 total_count;
	u32 first_index;
	u32 entry_count;
	u64 pending_mask;
	struct criu_snapshot_signal_queue_entry *entries;
};

struct criu_signal_capture {
	struct criu_snapshot_sigaction_entry actions[CRIU_SNAPSHOT_SIGACTION_COUNT];
	struct criu_signal_queue_chunk *queues;
	u32 queue_count;
};

int criu_collect_signals(struct criu_freeze_ctx *ctx,
			 struct criu_signal_capture *capture);
void criu_release_signals(struct criu_signal_capture *capture);

#endif
