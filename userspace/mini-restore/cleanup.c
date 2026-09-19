#include "cleanup.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

void b1_cleanup_init(struct b1_cleanup *cleanup)
{
	b1_carrier_manager_init(&cleanup->carriers);
	cleanup->staging = NULL;
	cleanup->restore_fd = -1;
}

void b1_cleanup_set_staging(struct b1_cleanup *cleanup,
			    struct b1_staging_plan *staging)
{
	cleanup->staging = staging;
}

void b1_cleanup_set_restore_fd(struct b1_cleanup *cleanup, int fd)
{
	cleanup->restore_fd = fd;
}

enum b1_restore_status b1_cleanup_record_pid(struct b1_cleanup *cleanup,
					     pid_t pid,
					     struct b1_restore_image *diag)
{
	return b1_carrier_manager_record(&cleanup->carriers, pid, diag);
}

void b1_cleanup_run(struct b1_cleanup *cleanup)
{
	size_t i;

	for (i = 0; i < cleanup->carriers.count; i++) {
		if (cleanup->carriers.pids[i] > 0) {
			int status;

			kill(cleanup->carriers.pids[i], SIGKILL);
			(void)waitpid(cleanup->carriers.pids[i], &status, WNOHANG);
		}
	}
	b1_carrier_manager_cleanup(&cleanup->carriers);
	b1_carrier_manager_free(&cleanup->carriers);
	if (cleanup->staging)
		b1_staging_plan_free(cleanup->staging);
	if (cleanup->restore_fd >= 0) {
		close(cleanup->restore_fd);
		cleanup->restore_fd = -1;
	}
}
