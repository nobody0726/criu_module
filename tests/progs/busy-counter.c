// SPDX-License-Identifier: GPL-2.0
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static volatile unsigned long counter;

static void on_term(int signo)
{
	(void)signo;
	stop = 1;
}

int main(void)
{
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	printf("pid=%ld\n", (long)getpid());
	fflush(stdout);
	while (!stop)
		counter++;
	return 0;
}
