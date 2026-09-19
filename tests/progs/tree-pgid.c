#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

static void stay(void)
{
	for (;;)
		pause();
}

int main(void)
{
	pid_t group_leader = fork();
	pid_t member;

	if (group_leader < 0)
		return 1;
	if (group_leader == 0) {
		if (setpgid(0, 0) < 0)
			_exit(2);
		stay();
	}
	if (setpgid(group_leader, group_leader) < 0)
		return 1;
	member = fork();
	if (member < 0)
		return 1;
	if (member == 0) {
		if (setpgid(0, group_leader) < 0)
			_exit(3);
		stay();
	}
	dprintf(STDOUT_FILENO, "READY root=%ld leader=%ld member=%ld\n",
		(long)getpid(), (long)group_leader, (long)member);
	fflush(stdout);
	stay();
}
