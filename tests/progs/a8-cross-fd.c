#define _GNU_SOURCE

#include <sys/socket.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>

#include "a5-fixture.h"

static int pipe_fds[2], sockets[2];
static int child_main(void *unused)
{
	char command;

	(void)unused;
	for (;;) {
		if (read(pipe_fds[0], &command, 1) == 1) {
			if (command != 'x' ||
			    write(sockets[1], "ok", 2) != 2)
				_exit(2);
			break;
		}
		usleep(10000);
	}
	puts("child-cross-check=PASS");
	fflush(stdout);
	fixture_alive();
	return 0;
}

int main(void)
{
	char *stack;
	pid_t child;
	char command, response[3] = { 0 };

	fixture_init();
	if (pipe2(pipe_fds, O_NONBLOCK) ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets))
		return 1;
	stack = malloc(65536);
	if (!stack)
		return 1;
	child = clone(child_main, stack + 65536, CLONE_FILES | SIGCHLD, NULL);
	if (child < 0)
		return 1;
	printf("READY root=%d child=%d\n", getpid(), child);
	fflush(stdout);
	while (read(STDIN_FILENO, &command, 1) != 1)
		usleep(10000);
	if (command != 'x' || write(pipe_fds[1], "x", 1) != 1)
		return 2;
	for (;;) {
		ssize_t n = read(sockets[0], response, 2);
		if (n == 2)
			break;
		if (n < 0)
			usleep(10000);
	}
	if (memcmp(response, "ok", 2))
		return 3;
	puts("cross-check=PASS pipe=PASS unix=PASS");
	fflush(stdout);
	fixture_alive();
}
