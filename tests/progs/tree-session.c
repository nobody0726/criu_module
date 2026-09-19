#define _GNU_SOURCE
#include <stdio.h>
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
	if (child == 0) {
		if (setsid() < 0)
			_exit(2);
		stay();
	}
	dprintf(STDOUT_FILENO, "READY root=%ld child=%ld\n",
		(long)getpid(), (long)child);
	fflush(stdout);
	stay();
}
