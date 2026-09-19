/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_SNAPSHOT_WRITER_H
#define CRIU_SNAPSHOT_WRITER_H

#include <linux/fs.h>
#include <linux/types.h>

#include "../../include/criu_snapshot.h"

struct criu_snapshot_writer {
	struct file *file;
	struct criu_snapshot_header header;
	char *path;
	char *tmp_path;
	loff_t pos;
	u64 total_size;
	u32 record_count;
	bool ended;
};

int criu_snapshot_writer_open(struct criu_snapshot_writer *writer,
			      const char *path,
			      const struct criu_snapshot_header *header);
int criu_snapshot_writer_record(struct criu_snapshot_writer *writer,
				 u16 type, u16 flags,
				 const void *payload, u64 length);
int criu_snapshot_writer_process_record(struct criu_snapshot_writer *writer,
					u32 owner_pid, u16 type, u16 flags,
					const void *payload, u64 length);
int criu_snapshot_writer_finish(struct criu_snapshot_writer *writer);
void criu_snapshot_writer_abort(struct criu_snapshot_writer *writer);

#endif
