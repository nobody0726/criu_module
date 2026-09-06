/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	long page = sysconf(_SC_PAGESIZE);
	char *base;
	int i;

	base = mmap(NULL, 4001 * page, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return 1;
	for (i = 0; i < 2000; i++)
		if (mprotect(base + (2 * i + 1) * page, page, PROT_READ))
			return 1;
	printf("pid=%d\n", getpid());
	fflush(stdout);
	for (;;)
		pause();
}
