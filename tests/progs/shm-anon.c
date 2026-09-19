#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "a5-fixture.h"

#define SHM_SIZE 8192
#define SHM_MAGIC 0xA8A8C0DEUL

struct shared_state {
	volatile unsigned long magic;
	volatile unsigned long parent_seq;
	volatile unsigned long child_seq;
	unsigned char pattern[SHM_SIZE - 3 * sizeof(unsigned long)];
};

static struct shared_state *state;

static int child_main(void)
{
	unsigned long seen = 0;
	size_t i;

	for (;;) {
		if (state->magic != SHM_MAGIC) {
			usleep(10000);
			continue;
		}
		if (state->parent_seq && state->parent_seq != seen) {
			for (i = 0; i < sizeof(state->pattern); i++) {
				if (state->pattern[i] != (unsigned char)(i ^ 0x5a))
					_exit(2);
			}
			seen = state->parent_seq;
			state->child_seq = seen;
			break;
		}
		usleep(10000);
	}
	puts("child-shmem-check=PASS");
	fflush(stdout);
	for (;;)
		sleep(1);
}

int main(void)
{
	pid_t child;
	char command;
	size_t i;

	fixture_init();
	state = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (state == MAP_FAILED)
		return 1;
	state->magic = SHM_MAGIC;
	for (i = 0; i < sizeof(state->pattern); i++)
		state->pattern[i] = (unsigned char)(i ^ 0x5a);
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		return child_main();
	printf("READY root=%d child=%d\n", getpid(), child);
	fflush(stdout);
	while (read(STDIN_FILENO, &command, 1) != 1)
		usleep(10000);
	if (command != 'x')
		return 2;
	state->parent_seq++;
	for (i = 0; i < 500; i++) {
		if (state->child_seq == state->parent_seq) {
			puts("shmem-check=PASS anon=PASS");
			fflush(stdout);
			fixture_alive();
		}
		usleep(10000);
	}
	return 3;
}
