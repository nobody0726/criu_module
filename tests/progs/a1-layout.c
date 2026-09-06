/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void *map(int fd, size_t size, int flags, int byte)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, fd, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	memset(p, byte, size);
	return p;
}

int main(void)
{
	long page = sysconf(_SC_PAGESIZE);
	void *anon, *shared, *file, *private, *ro, *guard, *split, *dont, *memfd;
	int fd, rfd;
	char name[] = "/tmp/a1 file XXXXXX";

	fd = mkstemp(name);
	if (fd < 0 || ftruncate(fd, page))
		return 1;
	file = map(fd, page, MAP_SHARED, 0x3c);
	private = map(fd, page, MAP_PRIVATE, 0x6d);
	rfd = open(name, O_RDONLY);
	if (rfd < 0)
		return 1;
	ro = mmap(NULL, page, PROT_READ, MAP_SHARED, rfd, 0);
	if (ro == MAP_FAILED || unlink(name))
		return 1;
	anon = map(-1, page, MAP_PRIVATE | MAP_ANONYMOUS, 0xa5);
	shared = map(-1, page, MAP_SHARED | MAP_ANONYMOUS, 0x5a);
	fd = memfd_create("a1-memfd", 0);
	if (fd < 0 || ftruncate(fd, page))
		return 1;
	memfd = map(fd, page, MAP_SHARED, 0x7e);

	guard = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	split = map(-1, 3 * page, MAP_PRIVATE | MAP_ANONYMOUS, 0x42);
	dont = map(-1, page, MAP_PRIVATE | MAP_ANONYMOUS, 0x99);
	if (guard == MAP_FAILED ||
	    mprotect((char *)split + page, page, PROT_READ) ||
	    madvise(dont, page, MADV_DONTDUMP))
		return 1;
	printf("pid=%d page=%ld anon=%lx shared=%lx file=%lx private=%lx ro=%lx "
	       "guard=%lx split=%lx dont=%lx memfd=%lx\n", getpid(), page,
	       (unsigned long)anon, (unsigned long)shared, (unsigned long)file,
	       (unsigned long)private, (unsigned long)ro, (unsigned long)guard,
	       (unsigned long)split, (unsigned long)dont, (unsigned long)memfd);
	fflush(stdout);
	for (;;)
		pause();
}
