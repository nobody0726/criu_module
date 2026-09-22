#define _GNU_SOURCE

#include "rst_session.h"

#include <errno.h>
#include <unistd.h>

int rst_restore_sid(const struct rst_item *item)
{
	pid_t current_sid;

	if (!item || item->pid <= 0 || item->sid <= 0)
		return -EINVAL;
	current_sid = getsid(0);
	if (current_sid < 0)
		return -errno;
	if (item->pid == item->sid) {
		if (current_sid != item->sid) {
			if (setsid() < 0)
				return -errno;
		}
		return getsid(0) == item->sid ? 0 : -EPERM;
	}
	return current_sid == item->sid ? 0 : -EOPNOTSUPP;
}

int rst_restore_pgid(struct rst_item *item, struct rst_shared *shared,
		     size_t leader_index, unsigned timeout_ms)
{
	pid_t current_pgid;
	int rc;

	if (!item || !shared || item->pid <= 0 || item->pgid <= 0)
		return -EINVAL;
	if (item->pid != item->pgid) {
		rc = rst_shared_wait_pgid(shared, leader_index, timeout_ms);
		if (rc)
			return rc;
	}
	current_pgid = getpgid(0);
	if (current_pgid < 0)
		return -errno;
	if (current_pgid != item->pgid && setpgid(0, item->pgid) < 0)
		return -errno;
	if (getpgid(0) != item->pgid)
		return -EPERM;
	if (item->pid == item->pgid)
		return rst_shared_mark_pgid(shared, leader_index);
	return 0;
}

int rst_mark_ready(struct rst_shared *shared, size_t index)
{
	return rst_shared_mark(shared, index, RST_TASK_READY);
}

int rst_wait_all_ready(struct rst_shared *shared, unsigned timeout_ms)
{
	if (!shared)
		return -EINVAL;
	return rst_shared_wait_count(shared, rst_shared_task_count(shared),
				     timeout_ms);
}

int rst_wait_commit_release(struct rst_shared *shared, unsigned timeout_ms)
{
	return rst_shared_wait_commit(shared, timeout_ms);
}
