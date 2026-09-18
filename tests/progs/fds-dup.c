#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "a5-fixture.h"

int main(void)
{
	int independent, shared, duplicate;
	int i;
	volatile char *mapping;
	const char *path = getenv("A5_REG_PATH");
	fixture_init();
	if (!path) path = "/tmp/a5-shared";
	independent = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	shared = open(path, O_RDWR);
	duplicate = fcntl(shared, F_DUPFD_CLOEXEC, 1100);
	if (independent < 0 || shared < 0 || duplicate < 0 ||
	    write(independent, "0123456789abcdef", 16) != 16 ||
	    lseek(independent, 3, SEEK_SET) != 3 ||
	    lseek(shared, 7, SEEK_SET) != 7)
		return 1;
	mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, independent, 0);
	if (mapping == MAP_FAILED || mapping[0] != '0') return 1;
	for (i = 10; i < 1034; i++)
		if (dup3(shared, i, i % 2 ? O_CLOEXEC : 0) != i) return 1;
	printf("pid=%d independent=%d shared=%d duplicate=%d\n",
	       getpid(), independent, shared, duplicate);
	fflush(stdout);
	fixture_command();
	for (i = 10; i < 1034; i++)
		if (fcntl(i, F_GETFD) != (i % 2 ? FD_CLOEXEC : 0) ||
		    lseek(i, 0, SEEK_CUR) != 7)
			return 3;
	if (lseek(independent, 0, SEEK_CUR) != 3 ||
	    fcntl(duplicate, F_GETFD) != FD_CLOEXEC || fcntl(shared, F_GETFD) != 0 ||
	    lseek(shared, 0, SEEK_CUR) != 7 ||
	    lseek(duplicate, 0, SEEK_CUR) != 7 ||
	    lseek(duplicate, 11, SEEK_SET) != 11 ||
	    lseek(shared, 0, SEEK_CUR) != 11 ||
	    lseek(independent, 0, SEEK_CUR) != 3 || mapping[1] != '1')
		return 2;
	puts("regular-check=PASS independent=3 shared=11 duplicate=11 sparse=1100 mmap=PASS descriptors=1024");
	fflush(stdout);
	fixture_alive();
}
