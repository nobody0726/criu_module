#ifndef A5_FIXTURE_H
#define A5_FIXTURE_H
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void fixture_init(void)
{
	struct sigaction action;
	int sig;
	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	for (sig = 1; sig < NSIG; sig++)
		if (sig != SIGKILL && sig != SIGSTOP)
			sigaction(sig, &action, NULL);
}

/* The harness appends only after restore, keeping recorded file sizes stable. */
static void fixture_command(void)
{
	char command;
	while (read(STDIN_FILENO, &command, 1) != 1)
		usleep(10000);
}

static void fixture_alive(void)
{
	unsigned round = 0;
	for (;;) {
		fixture_command();
		printf("alive=%u\n", ++round);
		fflush(stdout);
	}
}
#endif
