/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEAP_MAGIC 0xB100CAFEUL
#define STACK_MAGIC 0xB1005A7EUL

static __thread unsigned long tls_marker = 0xB1B1B1B1UL;
static volatile sig_atomic_t stop;

static void on_term(int signo)
{
	(void)signo;
	stop = 1;
}

int main(void)
{
	volatile unsigned long stack_marker = STACK_MAGIC;
	volatile unsigned long *heap_marker = malloc(sizeof(*heap_marker));
	unsigned long tick = 0;

	if (!heap_marker)
		return 1;
	*heap_marker = HEAP_MAGIC;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	printf("pid=%ld heap=%p stack=%p tls=%lx\n", (long)getpid(),
	       (const void *)heap_marker, (const void *)&stack_marker,
	       tls_marker);
	fflush(stdout);
	while (!stop) {
		if (*heap_marker != HEAP_MAGIC || stack_marker != STACK_MAGIC ||
		    tls_marker != 0xB1B1B1B1UL) {
			printf("marker=FAIL tick=%lu\n", tick);
			fflush(stdout);
			return 2;
		}
		printf("tick=%lu\n", tick++);
		fflush(stdout);
		usleep(100000);
	}
	free((void *)heap_marker);
	return 0;
}
