#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void marker(int signo)
{
	static const char msg[] = "MARKER\n";
	ssize_t written;

	(void)signo;
	written = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	(void)written;
}

static void stay(void)
{
	for (;;)
		pause();
}

int main(void)
{
	pid_t child = fork();
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = marker;
	sigaction(SIGUSR1, &sa, NULL);
	if (child < 0)
		return 1;
	if (child == 0)
		stay();
	dprintf(STDOUT_FILENO, "READY root=%ld child=%ld\n",
		(long)getpid(), (long)child);
	fflush(stdout);
	stay();
	return 0;
}
