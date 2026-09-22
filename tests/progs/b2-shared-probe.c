#define _GNU_SOURCE

#include "../../userspace/mini-restore/rst_shared.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	struct rst_shared *shared = NULL;
	pid_t child;
	int status;

	if (rst_shared_init(2, &shared))
		return 1;
	if (rst_shared_register_pid(shared, 0, getpid()))
		return 1;
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		usleep(20000);
		if (rst_shared_mark(shared, 1, RST_TASK_READY))
			_exit(2);
		_exit(0);
	}
	if (rst_shared_mark(shared, 0, RST_TASK_READY) ||
	    rst_shared_wait_count(shared, 2, 500) ||
	    rst_shared_release_commit(shared)) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		rst_shared_destroy(shared);
		return 1;
	}
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		rst_shared_destroy(shared);
		return 1;
	}
	if (rst_shared_wait_count(shared, 2, 20) != 0) {
		rst_shared_destroy(shared);
		return 1;
	}
	rst_shared_destroy(shared);

	if (rst_shared_init(1, &shared))
		return 1;
	if (rst_shared_wait_count(shared, 1, 20) == 0) {
		rst_shared_destroy(shared);
		return 1;
	}
	rst_shared_destroy(shared);
	printf("shared-ok\n");
	return 0;
}
