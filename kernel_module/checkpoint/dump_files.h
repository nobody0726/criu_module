/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_FILES_H
#define CRIU_DUMP_FILES_H

#include <linux/sched.h>
#include "../../include/criu_snapshot.h"
#include "../include/criu_kernel.h"
#include "snapshot_writer.h"

#define CRIU_SNAPSHOT_REC_FD 5
#define CRIU_SNAPSHOT_REC_FS 6

#define CRIU_FILE_PATH_MAX 512

struct criu_fd_record {
	struct criu_snapshot_fd_record abi;
} __attribute__((packed));

int criu_walk_fds(struct task_struct *task, criu_fd_fn fn, void *arg);

struct criu_fs_record {
	char cwd[CRIU_FILE_PATH_MAX];
	char root[CRIU_FILE_PATH_MAX];
} __attribute__((packed));

int criu_dump_files(struct task_struct *task,
			struct criu_snapshot_writer *writer);

#endif
