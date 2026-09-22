#define _GNU_SOURCE

#include "rst_session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	struct rst_shared *shared;
	struct rst_item leader = { .pgid = 0 };
	struct rst_item member = { .pgid = 0 };
	pid_t leader_pid;
	pid_t member_pid;
	int status;

	if (rst_shared_init(2, &shared) != 0)
		return 1;
	leader_pid = fork();
	if (leader_pid < 0)
		return 1;
	if (leader_pid == 0) {
		leader.pid = getpid();
		leader.pgid = leader.pid;
		if (rst_restore_pgid(&leader, shared, 0, 500) != 0 ||
		    rst_mark_ready(shared, 0) != 0)
			_exit(2);
		_exit(0);
	}
	if (rst_shared_register_pid(shared, 0, leader_pid) != 0)
		return 1;
	member_pid = fork();
	if (member_pid < 0)
		return 1;
	if (member_pid == 0) {
		member.pid = getpid();
		member.pgid = leader_pid;
		if (rst_restore_pgid(&member, shared, 0, 500) != 0 ||
		    rst_mark_ready(shared, 1) != 0)
			_exit(3);
		_exit(0);
	}
	member.pgid = leader_pid;
	if (rst_shared_register_pid(shared, 1, member_pid) != 0 ||
	    rst_wait_all_ready(shared, 1000) != 0)
		return 1;
	if (waitpid(leader_pid, &status, 0) != leader_pid ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 1;
	if (waitpid(member_pid, &status, 0) != member_pid ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 1;
	rst_shared_destroy(shared);

	if (rst_shared_init(1, &shared) != 0)
		return 1;
	if (rst_wait_all_ready(shared, 20) != -ETIMEDOUT)
		return 1;
	rst_shared_destroy(shared);
	printf("pgid-barrier-ok\n");
	return 0;
}
