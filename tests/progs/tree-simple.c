#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void stay(void)
{
	for (;;)
		pause();
}

int main(void)
{
	pid_t child = fork();
	if (child < 0)
		return 1;
	dprintf(STDOUT_FILENO, "READY root=%ld child=%ld\n",
		(long)getpid(), (long)child);
	fflush(stdout);
	stay();
	return 0;
}
