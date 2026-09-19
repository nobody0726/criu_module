#define _GNU_SOURCE

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "a5-fixture.h"

static int child(void *arg)
{
	(void)arg;
	printf("shared-fdt-child=%d\n", getpid());
	fflush(stdout);
	fixture_alive();
	return 0;
}

int main(void)
{
	char *stack;
	pid_t pid;

	fixture_init();
	stack = malloc(65536);
	if (!stack)
		return 2;
	pid = clone(child, stack + 65536, CLONE_FILES | SIGCHLD, NULL);
	if (pid < 0)
		return 2;
	printf("shared-fdt-parent=%d child=%d\n", getpid(), pid);
	fflush(stdout);
	fixture_alive();
	return 0;
}
