#include "../../userspace/mini-restore/carrier.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__aarch64__)
static unsigned long current_tls(void)
{
	unsigned long tls;

	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
	return tls;
}
#else
static unsigned long current_tls(void)
{
	return 0;
}
#endif

static int child_entry(void *arg)
{
	int fd = *(int *)arg;
	return write(fd, "C", 1) == 1 ? 42 : 43;
}

int main(void)
{
	struct b1_carrier_manager manager;
	struct b1_restore_image diag = {0};
	int fds[2], status;
	char marker = 0;
	pid_t target = 30000;

	if (pipe(fds))
		return 1;
	b1_carrier_manager_init(&manager);
	if (b1_create_exact_pid_carrier(&manager, target, current_tls(), child_entry,
				      &fds[1], &diag) != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", diag.diagnostic);
		return 1;
	}
	close(fds[1]);
	if (b1_wait_carrier(target, &status, &diag) != B1_RESTORE_OK)
		return 1;
	if (read(fds[0], &marker, 1) != 1)
		marker = 0;
	close(fds[0]);
	b1_carrier_manager_free(&manager);
	printf("B1_CARRIER_PROBE: status=%d signal=%d marker=%c\n", status,
	       WIFSIGNALED(status) ? WTERMSIG(status) : 0, marker ? marker : '-');
	return !(WIFEXITED(status) && WEXITSTATUS(status) == 42 && marker == 'C');
}
