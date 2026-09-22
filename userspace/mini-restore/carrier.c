#define _GNU_SOURCE

#include "carrier.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/sched.h>
#include <sys/syscall.h>
#if defined(__GLIBC__)
#include <sys/rseq.h>
#endif
#endif

#ifndef SYS_clone3
#ifdef __NR_clone3
#define SYS_clone3 __NR_clone3
#endif
#endif

#define B1_CARRIER_STACK_SIZE (1024U * 1024U)

#if defined(__linux__) && defined(SYS_clone3)
#if defined(__GLIBC__)
struct b1_carrier_child {
	b1_carrier_entry_fn entry;
	void *arg;
	void *rseq;
	unsigned int rseq_size;
};

static void *b1_carrier_current_rseq(void)
{
	if (!__rseq_size)
		return NULL;
	return (void *)((char *)__builtin_thread_pointer() + __rseq_offset);
}

static void b1_carrier_unregister_inherited_rseq(
		struct b1_carrier_child *child)
{
	if (!child->rseq || !child->rseq_size)
		return;
	(void)syscall(SYS_rseq, child->rseq, child->rseq_size,
			      RSEQ_FLAG_UNREGISTER, RSEQ_SIG);
}
#else
struct b1_carrier_child {
	b1_carrier_entry_fn entry;
	void *arg;
};

static void b1_carrier_unregister_inherited_rseq(
		struct b1_carrier_child *child)
{
	(void)child;
}
#endif
#endif

void b1_carrier_manager_init(struct b1_carrier_manager *manager)
{
	memset(manager, 0, sizeof(*manager));
}

void b1_carrier_manager_free(struct b1_carrier_manager *manager)
{
	free(manager->pids);
	b1_carrier_manager_init(manager);
}

enum b1_restore_status b1_carrier_manager_record(struct b1_carrier_manager *manager,
						 pid_t pid,
						 struct b1_restore_image *diag)
{
	pid_t *new_pids;

	if (manager->count == manager->capacity) {
		size_t new_capacity = manager->capacity ? manager->capacity * 2U : 4U;

		new_pids = realloc(manager->pids, new_capacity * sizeof(*manager->pids));
		if (!new_pids) {
			b1_restore_set_diag(diag, B1_RESTORE_IO, "recording carrier pid");
			return B1_RESTORE_IO;
		}
		manager->pids = new_pids;
		manager->capacity = new_capacity;
	}
	manager->pids[manager->count++] = pid;
	return B1_RESTORE_OK;
}

void b1_carrier_manager_cleanup(struct b1_carrier_manager *manager)
{
	size_t i;

	for (i = 0; i < manager->count; i++) {
		int status;

		if (manager->pids[i] <= 0)
			continue;
		kill(manager->pids[i], SIGKILL);
		while (waitpid(manager->pids[i], &status, 0) < 0 && errno == EINTR)
			;
		manager->pids[i] = -1;
	}
}

enum b1_restore_status b1_wait_carrier(pid_t pid, int *status,
				       struct b1_restore_image *diag)
{
	pid_t got;

	do {
		got = waitpid(pid, status, 0);
	} while (got < 0 && errno == EINTR);
	if (got != pid) {
		b1_restore_set_diag(diag, B1_RESTORE_IO, "waitpid failed for carrier");
		return B1_RESTORE_IO;
	}
	return B1_RESTORE_OK;
}

#if defined(__linux__) && defined(SYS_clone3)
static enum b1_restore_status clone_errno_status(int err,
						 struct b1_restore_image *diag)
{
	switch (err) {
	case EEXIST:
		b1_restore_set_diag(diag, B1_RESTORE_IO, "target pid already exists");
		return B1_RESTORE_IO;
	case EPERM:
		b1_restore_set_diag(diag, B1_RESTORE_UNSUPPORTED,
				    "clone3 set_tid requires privilege");
		return B1_RESTORE_UNSUPPORTED;
	case EINVAL:
		b1_restore_set_diag(diag, B1_RESTORE_UNSUPPORTED,
				    "clone3 set_tid unsupported by this kernel");
		return B1_RESTORE_UNSUPPORTED;
	default:
		b1_restore_set_diag(diag, B1_RESTORE_IO, "clone3 failed");
		return B1_RESTORE_IO;
	}
}
#endif

enum b1_restore_status b1_create_exact_pid_carrier_flags(
	struct b1_carrier_manager *manager, pid_t target_pid, uint64_t tls,
	unsigned flags, b1_carrier_entry_fn entry, void *arg,
	struct b1_restore_image *diag)
{
#if defined(__linux__) && defined(SYS_clone3)
	void *stack = NULL;
	struct b1_carrier_child child = {
		.entry = entry,
		.arg = arg,
#if defined(__GLIBC__)
		.rseq = b1_carrier_current_rseq(),
		.rseq_size = __rseq_size,
#endif
	};
	struct clone_args args = {
		/* normal B1 carriers use .flags = CLONE_SETTLS; tree coordinators
		 * defer TLS installation until bootstrap after libc work is done. */
		.flags = ((flags & B1_CARRIER_F_KEEP_PARENT_TLS) ? 0 :
			  CLONE_SETTLS) |
			((flags & B1_CARRIER_F_CLONE_PARENT) ? CLONE_PARENT : 0),
		.exit_signal = SIGCHLD,
		.stack = 0,
		.stack_size = 0,
		.tls = (flags & B1_CARRIER_F_KEEP_PARENT_TLS) ? 0 : tls,
		.set_tid = (unsigned long)&target_pid,
		.set_tid_size = 1,
	};
	long rc;

	if (target_pid <= 0 || !entry) {
		b1_restore_set_diag(diag, B1_RESTORE_FORMAT, "bad carrier request");
		return B1_RESTORE_FORMAT;
	}
	stack = malloc(B1_CARRIER_STACK_SIZE);
	if (!stack) {
		b1_restore_set_diag(diag, B1_RESTORE_IO, "allocating carrier stack");
		return B1_RESTORE_IO;
	}
	args.stack = (unsigned long)stack;
	args.stack_size = B1_CARRIER_STACK_SIZE;
	/*
	 * Linux rejects CLONE_PARENT together with a non-zero exit signal in
	 * clone3().  A sibling restore has no waitable parent here, so let
	 * the kernel reparent it on exit; ordinary carriers retain SIGCHLD.
	 */
	if (flags & B1_CARRIER_F_CLONE_PARENT)
		args.exit_signal = 0;

	rc = syscall(SYS_clone3, &args, sizeof(args));
	if (rc < 0) {
		int err = errno;

		free(stack);
		return clone_errno_status(err, diag);
	}
	if (rc == 0) {
		int child_rc;

		b1_carrier_unregister_inherited_rseq(&child);
		child_rc = child.entry(child.arg);

		_exit(child_rc < 0 ? 127 : child_rc);
	}
	free(stack);
	if ((pid_t)rc != target_pid) {
		kill((pid_t)rc, SIGKILL);
		(void)b1_wait_carrier((pid_t)rc, NULL, diag);
		b1_restore_set_diag(diag, B1_RESTORE_IO, "target pid mismatch");
		return B1_RESTORE_IO;
	}
	return b1_carrier_manager_record(manager, (pid_t)rc, diag);
#else
	(void)manager;
	(void)target_pid;
	(void)tls;
	(void)flags;
	(void)entry;
	(void)arg;
	b1_restore_set_diag(diag, B1_RESTORE_UNSUPPORTED,
			    "clone3 set_tid is only available on Linux");
	return B1_RESTORE_UNSUPPORTED;
#endif
}

enum b1_restore_status b1_create_exact_pid_carrier(
	struct b1_carrier_manager *manager, pid_t target_pid, uint64_t tls,
	b1_carrier_entry_fn entry, void *arg, struct b1_restore_image *diag)
{
	return b1_create_exact_pid_carrier_flags(manager, target_pid, tls, 0,
						 entry, arg, diag);
}
