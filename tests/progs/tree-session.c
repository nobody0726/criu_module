#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int marker_fd = STDOUT_FILENO;

static void marker(int signo)
{
	static const char msg[] = "MARKER\n";
	ssize_t written;

	(void)signo;
	written = write(marker_fd, msg, sizeof(msg) - 1);
	(void)written;
}

static void stay(void)
{
	/*
	 * Keep the checkpoint point in user space.  The B1 bootstrap contract
	 * also requires the restored stack marker at sp+8 and x2 == 0; encode
	 * both directly in the loop so B2 exercises tree/session ordering on
	 * top of the already-validated B1 handoff.
	 */
#if defined(__aarch64__)
	__asm__ volatile(
		"sub sp, sp, #32\n"
		"mov x0, #0x5a7e\n"
		"movk x0, #0xb100, lsl #16\n"
		"str x0, [sp, #8]\n"
		"mov x2, xzr\n"
		"1: b 1b\n");
	__builtin_unreachable();
#else
	for (;;)
		;
#endif
}

int main(void)
{
	const char *marker_file = getenv("A7_MARKER_FILE");
	pid_t child = fork();
	struct sigaction sa;

	if (marker_file) {
		marker_fd = open(marker_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (marker_fd < 0)
			return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = marker;
	sigaction(SIGUSR1, &sa, NULL);
	if (child < 0)
		return 1;
	if (child == 0) {
		if (setsid() < 0)
			_exit(2);
		stay();
	}
	dprintf(STDOUT_FILENO, "READY root=%ld child=%ld\n",
		(long)getpid(), (long)child);
	fflush(stdout);
	stay();
}
