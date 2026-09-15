/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NTHREADS 8

static __thread unsigned long tls_magic;
static volatile unsigned long counters[NTHREADS];

static void reset_signal_dispositions(void)
{
	struct sigaction action;
	int signo;

	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	for (signo = 1; signo < NSIG; signo++)
		if (signo != SIGKILL && signo != SIGSTOP)
			sigaction(signo, &action, NULL);
}

static void *worker(void *arg)
{
	unsigned long index = (unsigned long)(uintptr_t)arg;

	tls_magic = 0x71500000UL + index;
	for (;;) {
		counters[index]++;
		sleep(1);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[NTHREADS];
	unsigned long i;

	printf("pid=%ld nthreads=%d\n", (long)getpid(), NTHREADS + 1);
	fflush(stdout);
	for (i = 0; i < NTHREADS; i++)
		if (pthread_create(&threads[i], NULL, worker,
				   (void *)(uintptr_t)i) != 0)
			return 1;
	reset_signal_dispositions();
	for (i = 0; i < NTHREADS; i++)
		printf("thread=%lu tls=%lx\n", i, 0x71500000UL + i);
	fflush(stdout);
	for (;;) {
		sleep(1);
	}
}
