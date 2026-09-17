#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int p[2];
	char buf[32];
	if (pipe(p) < 0)
		return 1;
	if (write(p[1], "pipe-data", 9) != 9)
		return 1;
	printf("pid=%d read_fd=%d write_fd=%d\n", getpid(), p[0], p[1]);
	fflush(stdout);
	for (;;) {
		if (read(p[0], buf, sizeof(buf)) < 0 && errno != EAGAIN)
			return 2;
		sleep(1);
	}
}
