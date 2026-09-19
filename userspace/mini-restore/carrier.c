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
#endif

#ifndef SYS_clone3
#ifdef __NR_clone3
#define SYS_clone3 __NR_clone3
#endif
#endif

#define B1_CARRIER_STACK_SIZE (1024U * 1024U)

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

enum b1_restore_status b1_create_exact_pid_carrier(struct b1_carrier_manager *manager,
						  pid_t target_pid,
						  b1_carrier_entry_fn entry,
						  void *arg,
						  struct b1_restore_image *diag)
{
#if defined(__linux__) && defined(SYS_clone3)
	void *stack = NULL;
	struct clone_args args = {
		.flags = 0,
		.exit_signal = SIGCHLD,
		.stack = 0,
		.stack_size = 0,
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

	rc = syscall(SYS_clone3, &args, sizeof(args));
	if (rc < 0) {
		int err = errno;

		free(stack);
		return clone_errno_status(err, diag);
	}
	if (rc == 0) {
		int child_rc = entry(arg);

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
	(void)entry;
	(void)arg;
	b1_restore_set_diag(diag, B1_RESTORE_UNSUPPORTED,
			    "clone3 set_tid is only available on Linux");
	return B1_RESTORE_UNSUPPORTED;
#endif
}
