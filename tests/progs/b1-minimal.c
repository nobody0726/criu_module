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

static __thread volatile unsigned long tls_marker = 0xB1B1B1B1UL;
static volatile sig_atomic_t stop;

static __attribute__((noinline)) unsigned long read_tls_marker(void)
{
	return tls_marker;
}

static void on_term(int signo)
{
	(void)signo;
	stop = 1;
}

int main(void)
{
	volatile unsigned long stack_marker = STACK_MAGIC;
	volatile unsigned long *heap_marker = malloc(sizeof(*heap_marker));
	volatile unsigned long tick = 0;

	if (!heap_marker)
		return 1;
	*heap_marker = HEAP_MAGIC;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	printf("pid=%ld heap=%p stack=%p tls=%lx\n", (long)getpid(),
	       (const void *)heap_marker, (const void *)&stack_marker,
	       read_tls_marker());
	fflush(stdout);
	while (!stop) {
		if (*heap_marker != HEAP_MAGIC || stack_marker != STACK_MAGIC ||
		    read_tls_marker() != 0xB1B1B1B1UL) {
			printf("marker=FAIL tick=%lu\n", tick);
			fflush(stdout);
			return 2;
		}
		tick++;
		/* Keep the dump point in ordinary user code, outside a syscall. */
		asm volatile("yield" ::: "memory");
	}
	free((void *)heap_marker);
	return 0;
}
