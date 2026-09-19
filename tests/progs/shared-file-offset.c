#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "a5-fixture.h"

int main(int argc, char **argv)
{
	char byte;
	int fd;

	if (argc != 2)
		return 1;
	fixture_init();
	fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, "ab", 2) != 2 || lseek(fd, 0, SEEK_SET) < 0)
		return 2;
	if (read(fd, &byte, 1) != 1 || byte != 'a')
		return 3;
	printf("shared-offset pid=%d pos=%lld\n", getpid(),
	       (long long)lseek(fd, 0, SEEK_CUR));
	fflush(stdout);
	fixture_alive();
	return 0;
}
