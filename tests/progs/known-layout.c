/* Keep a small, recognizable address-space layout for S0 probes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define PRIVATE_BYTE 0xa5
#define SHARED_BYTE 0x5a

static int fill_page(void *mapping, size_t length, unsigned char value)
{
	unsigned char *bytes = mapping;
	size_t i;

	memset(mapping, value, length);
	for (i = 0; i < length; i++) {
		if (bytes[i] != value)
			return -1;
	}
	return 0;
}

int main(void)
{
	void *anon = MAP_FAILED;
	void *shared = MAP_FAILED;
	void *guard = MAP_FAILED;
	void *insert = MAP_FAILED;
	long page_size;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return EXIT_FAILURE;

	anon = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anon == MAP_FAILED ||
	    fill_page(anon, (size_t)page_size, PRIVATE_BYTE) != 0)
		goto fail;

	shared = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED ||
	    fill_page(shared, (size_t)page_size, SHARED_BYTE) != 0)
		goto fail;

	guard = mmap(NULL, (size_t)page_size, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guard == MAP_FAILED)
		goto fail;

	insert = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (insert == MAP_FAILED)
		goto fail;

	if (printf("pid=%ld anon=%p shared=%p guard=%p insert=%p\n",
		   (long)getpid(), anon, shared, guard, insert) < 0 ||
	    fflush(stdout) != 0)
		goto fail;

	for (;;) {
		(void)pause();
	}

fail:
	if (insert != MAP_FAILED)
		(void)munmap(insert, (size_t)page_size);
	if (guard != MAP_FAILED)
		(void)munmap(guard, (size_t)page_size);
	if (shared != MAP_FAILED)
		(void)munmap(shared, (size_t)page_size);
	if (anon != MAP_FAILED)
		(void)munmap(anon, (size_t)page_size);
	return EXIT_FAILURE;
}
