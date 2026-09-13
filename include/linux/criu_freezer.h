/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRIU_FREEZER_H
#define _LINUX_CRIU_FREEZER_H

#include <linux/sched.h>
#include <linux/types.h>

struct criu_freezer_cookie;

int criu_cgroup_freeze_threadgroup(struct task_struct *leader,
				   struct criu_freezer_cookie **cookie,
				   char *original_path, size_t original_len,
				   char *temporary_path, size_t temporary_len);
int criu_cgroup_thaw_threadgroup(struct criu_freezer_cookie *cookie);

#endif
