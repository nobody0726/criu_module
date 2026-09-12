/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_PAGE_SCAN_H
#define CRIU_PAGE_SCAN_H

#include <linux/mm.h>
#include <linux/sched.h>

#include "snapshot_writer.h"

/* PAGE_RUN payload is self describing and followed by inline page bytes. */
struct criu_page_run_record {
	__u64 start;
	__u32 nr_pages;
	__u32 page_size;
	__u32 flags;
	__u32 payload_bytes;
} __attribute__((packed));

#define CRIU_PAGE_RUN_PRESENT  (1U << 0)
#define CRIU_PAGE_RUN_VDSO     (1U << 1)
#define CRIU_PAGE_RUN_ZERO     (1U << 2)

int criu_dump_pages(struct task_struct *task,
			struct criu_snapshot_writer *writer);

#endif
