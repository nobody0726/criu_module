// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static volatile uint64_t counters[4];

static void on_term(int signo)
{
	(void)signo;
	stop = 1;
}

static void *worker(void *arg)
{
	size_t index = (size_t)(uintptr_t)arg;

	while (!stop) {
		counters[index]++;
		usleep(1000);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[4];
	size_t i;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	for (i = 0; i < 4; i++) {
		if (pthread_create(&threads[i], NULL, worker,
				   (void *)(uintptr_t)i) != 0)
			return 1;
	}
	printf("pid=%ld threads=5\n", (long)getpid());
	fflush(stdout);
	while (!stop)
		sleep(1);
	for (i = 0; i < 4; i++)
		pthread_join(threads[i], NULL);
	return 0;
}
