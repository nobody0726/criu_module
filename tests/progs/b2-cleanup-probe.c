#define _GNU_SOURCE

#include "rst_cleanup.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	struct rst_shared *shared;
	pid_t child;
	int status;

	if (rst_shared_init(1, &shared) != 0)
		return 1;
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		for (;;)
			pause();
	}
	if (rst_shared_register_pid(shared, 0, child) != 0)
		return 1;
	if (rst_cleanup_all(shared, NULL, 1000) != 0)
		return 1;
	if (waitpid(child, &status, WNOHANG) > 0)
		return 1;
	if (kill(child, 0) == 0 || errno != ESRCH)
		return 1;
	rst_shared_destroy(shared);
	printf("cleanup-ok\n");
	return 0;
}
