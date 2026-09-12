/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_MM_H
#define CRIU_DUMP_MM_H

#include <linux/sched.h>
#include <linux/mm.h>
#include "criu_kernel.h"
#include "snapshot_writer.h"
#include "../../include/criu_snapshot.h"

#ifndef CRIU_SNAPSHOT_REC_MM
#define CRIU_SNAPSHOT_REC_MM 2
#endif
#ifndef CRIU_SNAPSHOT_REC_VMA
#define CRIU_SNAPSHOT_REC_VMA 3
#endif

enum criu_vma_dump_policy {
	CRIU_VMA_DUMP_PRIVATE_ANON = 1,
	CRIU_VMA_DUMP_PRIVATE_FILE = 2,
	CRIU_VMA_DUMP_VDSO = 3,
	CRIU_VMA_DUMP_VVAR = 4,
	CRIU_VMA_DUMP_SKIP_GUARD = 5,
	CRIU_VMA_DUMP_SKIP_DONTDUMP = 6,
};

/* Semantic records consumed by the user-space converter. */
struct criu_mm_record {
	__u32 pid, tgid;
	__u64 total_vm;
	__u64 start_code, end_code, start_data, end_data;
	__u64 start_brk, brk, start_stack;
	__u64 arg_start, arg_end, env_start, env_end;
	__u32 vma_count;
	__u32 reserved;
} __attribute__((packed));

struct criu_vma_record {
	__u64 start, end, pgoff;
	__u32 prot; /* CRIU-style PROT_READ/WRITE/EXEC bits: 1/2/4. */
	__u32 class;
	__u32 special;
	__u32 dump_policy;
	__u32 flags; /* shared, growsdown, dontdump, locked semantic bits. */
	__u64 dev, ino;
	__u64 pages_present, pages_saved, pages_skipped_zero, pages_skipped_file;
	char path[CRIU_PATH_MAX];
} __attribute__((packed));

int criu_dump_mm(struct task_struct *task,
		 struct criu_snapshot_writer *writer);

#endif
