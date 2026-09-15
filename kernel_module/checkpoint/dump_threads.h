/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_THREADS_H
#define CRIU_DUMP_THREADS_H

#include <linux/sched.h>
#include <linux/types.h>

#include "snapshot_writer.h"

/* RCU protects enumeration only; callbacks must not sleep or perform I/O. */
int criu_dump_threads(struct task_struct *leader,
			struct criu_snapshot_writer *writer);

#endif
