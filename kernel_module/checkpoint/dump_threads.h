/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_THREADS_H
#define CRIU_DUMP_THREADS_H

#include <linux/sched.h>
#include <linux/types.h>

#include "snapshot_writer.h"

struct criu_freeze_ctx;

/* RCU protects enumeration only; callbacks must not sleep or perform I/O. */
int criu_dump_threads(struct task_struct *leader,
			struct criu_snapshot_writer *writer);
int criu_dump_process_threads(struct criu_freeze_ctx *ctx,
			      unsigned int process_index,
			      struct criu_snapshot_writer *writer);

#endif
