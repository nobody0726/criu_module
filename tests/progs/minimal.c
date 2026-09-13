/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A3's intentionally small checkpoint target.
 *
 * The test harness opens all three standard descriptors as regular files
 * before starting this program.  Keeping that requirement explicit catches
 * accidental expansion of the A3 file-descriptor contract.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

#define HEAP_SIZE 8192U
#define STACK_SIZE 4096U
#define HEAP_PATTERN 0x3cU
#define STACK_PATTERN 0xc3U

static int check_regular_fds(void)
{
	struct stat st;
	int fd;

	for (fd = 0; fd <= 2; fd++) {
		if (fstat(fd, &st) < 0)
			return -1;
		if (!S_ISREG(st.st_mode)) {
			errno = ESPIPE;
			return -1;
		}
	}
	return 0;
}

static int check_bytes(const volatile unsigned char *buf, size_t len,
			       unsigned char expected)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] != expected)
			return -1;
	return 0;
}

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

int main(void)
{
	volatile unsigned char stack_marker[STACK_SIZE];
	volatile unsigned char *heap_marker;
	unsigned char command;
	unsigned long tick = 0;

	reset_signal_dispositions();

	if (check_regular_fds() < 0) {
		fprintf(stderr, "minimal: fd 0/1/2 must be regular files: %s\n",
			strerror(errno));
		return 64;
	}

	heap_marker = malloc(HEAP_SIZE);
	if (!heap_marker)
		return 1;
	memset((void *)heap_marker, HEAP_PATTERN, HEAP_SIZE);
	memset((void *)stack_marker, STACK_PATTERN, sizeof(stack_marker));

	printf("pid=%ld heap=%p stack=%p\n", (long)getpid(),
	       (const void *)heap_marker, (const void *)stack_marker);
	printf("tick=0\n");
	fflush(stdout);

	for (;;) {
		if (check_bytes(heap_marker, HEAP_SIZE, HEAP_PATTERN) < 0) {
			printf("HEAP CORRUPT tick=%lu\n", tick);
			fflush(stdout);
			free((void *)heap_marker);
			return 2;
		}
		if (check_bytes(stack_marker, sizeof(stack_marker), STACK_PATTERN) < 0) {
			printf("STACK CORRUPT tick=%lu\n", tick);
			fflush(stdout);
			free((void *)heap_marker);
			return 3;
		}
		/*
		 * Keep all three regular-file metadata values stable while the
		 * snapshot is collected.  The harness appends one byte to stdin
		 * after restore; that byte is the explicit request to report the
		 * in-memory tick and prove that execution resumed.
		 */
		if (read(STDIN_FILENO, &command, sizeof(command)) == 1) {
			printf("tick=%lu\n", tick);
			fflush(stdout);
		}
		tick++;
		usleep(10000);
	}
}
