#define _GNU_SOURCE

#include "rst_cleanup.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static int deadline_after(unsigned timeout_ms, struct timespec *deadline)
{
	if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0)
		return -errno;
	deadline->tv_sec += timeout_ms / 1000U;
	deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
	return 0;
}

static int deadline_expired(const struct timespec *deadline)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 1;
	return now.tv_sec > deadline->tv_sec ||
	       (now.tv_sec == deadline->tv_sec &&
		now.tv_nsec >= deadline->tv_nsec);
}

static int pid_gone(pid_t pid)
{
	char path[64];
	char buf[256];
	int fd;
	ssize_t got;

	if (kill(pid, 0) == 0)
		goto inspect_proc;
	if (errno == ESRCH)
		return 1;
	return 0;
inspect_proc:
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	got = read(fd, buf, sizeof(buf) - 1U);
	close(fd);
	if (got <= 0)
		return 0;
	buf[got] = '\0';
	{
		char *end_name = strrchr(buf, ')');

		if (end_name && end_name[1] == ' ' && end_name[2] == 'Z')
			return 1;
	}
	return 0;
}

int rst_wait_tree_root(pid_t root_pid, unsigned timeout_ms)
{
	struct timespec deadline;

	if (root_pid <= 0)
		return -EINVAL;
	if (deadline_after(timeout_ms, &deadline))
		return -errno;
	for (;;) {
		if (!pid_gone(root_pid))
			return 0;
		if (kill(root_pid, 0) < 0 && errno != ESRCH)
			return -errno;
		if (pid_gone(root_pid))
			return -ESRCH;
		if (deadline_expired(&deadline))
			return -ETIMEDOUT;
		usleep(1000);
	}
}

int rst_cleanup_all(struct rst_shared *shared,
		    const struct rst_pstree *tree,
		    unsigned timeout_ms)
{
	struct timespec deadline;
	size_t i;

	(void)tree;
	if (!shared)
		return -EINVAL;
	if (deadline_after(timeout_ms, &deadline))
		return -errno;
	(void)rst_shared_abort(shared, -ECANCELED);
	for (i = 0; i < rst_shared_task_count(shared); i++) {
		pid_t pid;

		if (rst_shared_pid(shared, i, &pid) != 0)
			continue;
		if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
			return -errno;
	}
	for (;;) {
		int any_live = 0;

		for (i = 0; i < rst_shared_task_count(shared); i++) {
			pid_t pid;
			int status;

			if (rst_shared_pid(shared, i, &pid) != 0)
				continue;
			while (waitpid(pid, &status, WNOHANG) < 0 && errno == EINTR)
				;
			if (!pid_gone(pid))
				any_live = 1;
		}
		if (!any_live)
			return 0;
		if (deadline_expired(&deadline))
			return -ETIMEDOUT;
		usleep(1000);
	}
}
