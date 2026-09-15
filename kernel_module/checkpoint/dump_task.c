/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/string.h>

#include "dump_task.h"

#define CRIU_SNAPSHOT_UNSUPPORTED (-EOPNOTSUPP)

static bool has_signal_handlers(struct sighand_struct *sighand)
{
	int i;

	if (!sighand)
		return true;
	for (i = 0; i < _NSIG; i++)
		if (sighand->action[i].sa.sa_handler != SIG_DFL)
			return true;
	return false;
}

static bool has_timers(struct signal_struct *sig)
{
	/* posix_timers is the kernel's task-owned timer list on 5.10. */
	return !sig || !list_empty(&sig->posix_timers);
}

int criu_dump_task(struct task_struct *task,
			struct criu_snapshot_writer *writer)
{
	struct criu_task_record rec;
	struct criu_regs_record regs;
	struct criu_creds_record creds;
	const struct cred *cred;
	struct task_struct *parent;
	int i;

	if (!task || !writer)
		return -EINVAL;
	if (task->sighand && has_signal_handlers(task->sighand)) {
		pr_info("criu_dump_task: reject signal handlers pid=%d\n",
			task_pid_vnr(task));
		return -EOPNOTSUPP;
	}
	if (task->sighand && has_timers(task->signal)) {
		pr_info("criu_dump_task: reject timers pid=%d\n", task_pid_vnr(task));
		return -EOPNOTSUPP;
	}
	if (signal_pending(task) || (task->pending.signal.sig[0])) {
		pr_info("criu_dump_task: reject pending signal pid=%d pending=%lx\n",
			task_pid_vnr(task), task->pending.signal.sig[0]);
		return -EOPNOTSUPP;
	}

	memset(&rec, 0, sizeof(rec));
	rec.pid = task_pid_nr(task);
	rec.tgid = task_tgid_nr(task);
	rcu_read_lock();
	parent = rcu_dereference(task->real_parent);
	rec.ppid = parent ? task_pid_nr(parent) : 0;
	rcu_read_unlock();
	rec.task_flags = READ_ONCE(task->flags);
	rec.state = READ_ONCE(task->state);
	cred = get_task_cred(task);
	rec.uid = from_kuid(&init_user_ns, cred->uid);
	rec.gid = from_kgid(&init_user_ns, cred->gid);
	rec.euid = from_kuid(&init_user_ns, cred->euid);
	rec.egid = from_kgid(&init_user_ns, cred->egid);
	creds.uid = rec.uid;
	creds.gid = rec.gid;
	creds.euid = rec.euid;
	creds.egid = rec.egid;
	creds.suid = from_kuid(&init_user_ns, cred->suid);
	creds.sgid = from_kgid(&init_user_ns, cred->sgid);
	creds.fsuid = from_kuid(&init_user_ns, cred->fsuid);
	creds.fsgid = from_kgid(&init_user_ns, cred->fsgid);
	creds.securebits = cred->securebits;
	memcpy(creds.cap_inheritable, cred->cap_inheritable.cap,
	       sizeof(creds.cap_inheritable));
	memcpy(creds.cap_permitted, cred->cap_permitted.cap,
	       sizeof(creds.cap_permitted));
	memcpy(creds.cap_effective, cred->cap_effective.cap,
	       sizeof(creds.cap_effective));
	memcpy(creds.cap_bset, cred->cap_bset.cap, sizeof(creds.cap_bset));
	memset(creds.cap_ambient, 0, sizeof(creds.cap_ambient));
	put_cred(cred);
	memcpy(rec.blocked, &task->blocked, sizeof(task->blocked));
	memcpy(rec.pending, &task->pending.signal, sizeof(task->pending.signal));
	if (task->signal)
		memcpy(rec.shared_pending, &task->signal->shared_pending.signal,
		       sizeof(task->signal->shared_pending.signal));
	if (task->signal) {
		rec.rlimit_count = RLIM_NLIMITS;
		for (i = 0; i < RLIM_NLIMITS; i++) {
			rec.rlimits[i][0] = task->signal->rlim[i].rlim_cur;
			rec.rlimits[i][1] = task->signal->rlim[i].rlim_max;
		}
	}
	get_task_comm(rec.comm, task);
	if (!task_pt_regs(task)) {
		pr_info("criu_dump_task: reject missing regs pid=%d\n",
			task_pid_vnr(task));
		return -EOPNOTSUPP;
	}
	memset(&regs, 0, sizeof(regs));
	regs.size = sizeof(struct pt_regs);
	memcpy(regs.data, task_pt_regs(task), sizeof(struct pt_regs));
#ifdef CONFIG_ARM64
	/* The architecture preserves TPIDR_EL0 in thread.uw, outside pt_regs. */
	regs.tls = task->thread.uw.tp_value;
#endif
	if (criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_TASK,
					   0, &rec, sizeof(rec)))
		return -EIO;
	if (criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_REGS,
					   0, &regs, sizeof(regs)))
		return -EIO;
	return criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_CREDS,
					   0, &creds, sizeof(creds));
}
