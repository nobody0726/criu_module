#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int marker_fd = STDOUT_FILENO;

static void marker(int signo)
{
	static const char msg[] = "MARKER\n";
	ssize_t written;

	(void)signo;
	written = write(marker_fd, msg, sizeof(msg) - 1);
	(void)written;
}

static void stay(void)
{
	for (;;)
		pause();
}

int main(void)
{
	const char *marker_file = getenv("A7_MARKER_FILE");
	pid_t group_leader = fork();
	pid_t member;
	struct sigaction sa;

	if (marker_file) {
		marker_fd = open(marker_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (marker_fd < 0)
			return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = marker;
	sigaction(SIGUSR1, &sa, NULL);
	if (group_leader < 0)
		return 1;
	if (group_leader == 0) {
		if (setpgid(0, 0) < 0)
			_exit(2);
		stay();
	}
	if (setpgid(group_leader, group_leader) < 0)
		return 1;
	member = fork();
	if (member < 0)
		return 1;
	if (member == 0) {
		if (setpgid(0, group_leader) < 0)
			_exit(3);
		stay();
	}
	dprintf(STDOUT_FILENO, "READY root=%ld leader=%ld member=%ld\n",
		(long)getpid(), (long)group_leader, (long)member);
	fflush(stdout);
	stay();
}
