/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_TIMERS_H
#define CRIU_DUMP_TIMERS_H

#include <linux/types.h>

#include "../../include/criu_snapshot.h"

struct criu_freeze_ctx;
struct criu_snapshot_writer;

struct criu_timer_capture {
	struct criu_snapshot_itimer_entry itimers[CRIU_SNAPSHOT_ITIMER_COUNT];
	struct criu_snapshot_posix_timer_entry *posix;
	u32 posix_count;
};

int criu_collect_timers(struct criu_freeze_ctx *ctx,
			struct criu_timer_capture *capture);
int criu_collect_process_timers(struct criu_freeze_ctx *ctx,
				unsigned int process_index,
				struct criu_timer_capture *capture);
int criu_emit_timers(const struct criu_timer_capture *capture,
		     struct criu_snapshot_writer *writer);
void criu_release_timers(struct criu_timer_capture *capture);

#endif
