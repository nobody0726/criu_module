/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_TASK_H
#define CRIU_DUMP_TASK_H

#include <linux/sched.h>
#include <linux/types.h>

#include "../../include/criu_snapshot.h"
#include "snapshot_writer.h"

#define CRIU_SNAPSHOT_REC_TASK 1
#define CRIU_SNAPSHOT_REC_REGS 4
#define CRIU_SNAPSHOT_REC_CREDS 7

#define CRIU_TASK_SIG_BYTES  sizeof(sigset_t)
#define CRIU_TASK_REG_BYTES  sizeof(struct pt_regs)

struct criu_task_record {
	__u32 pid, tgid, ppid;
	__u32 uid, gid, euid, egid;
	__u64 task_flags;
	__u64 state;
	__u32 rlimit_count;
	__u8 blocked[CRIU_TASK_SIG_BYTES];
	__u8 pending[CRIU_TASK_SIG_BYTES];
	__u8 shared_pending[CRIU_TASK_SIG_BYTES];
	__u64 rlimits[RLIM_NLIMITS][2];
} __attribute__((packed));

struct criu_regs_record {
	__u32 size;
	__u8 data[CRIU_TASK_REG_BYTES];
} __attribute__((packed));

struct criu_creds_record {
	__u32 uid;
	__u32 gid;
	__u32 euid;
	__u32 egid;
	__u32 suid;
	__u32 sgid;
	__u32 fsuid;
	__u32 fsgid;
	__u32 securebits;
	__u32 cap_inheritable[2];
	__u32 cap_permitted[2];
	__u32 cap_effective[2];
	__u32 cap_bset[2];
	__u32 cap_ambient[2];
} __attribute__((packed));

int criu_dump_task(struct task_struct *task,
			struct criu_snapshot_writer *writer);

#endif
