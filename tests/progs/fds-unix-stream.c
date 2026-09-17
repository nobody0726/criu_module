#define _GNU_SOURCE
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int s[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) < 0)
		return 1;
	if (write(s[0], "unix-data", 9) != 9)
		return 1;
	printf("pid=%d left_fd=%d right_fd=%d\n", getpid(), s[0], s[1]);
	fflush(stdout);
	for (;;)
		sleep(1);
}
