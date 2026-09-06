// SPDX-License-Identifier: GPL-2.0
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_term(int signo)
{
	(void)signo;
	stop = 1;
}

int main(void)
{
	signal(SIGTERM, on_term);
	printf("pid=%ld\n", (long)getpid());
	fflush(stdout);
	while (!stop)
		pause();
	return 0;
}
