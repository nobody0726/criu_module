#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	volatile int *shared = mmap(NULL, sizeof(*shared),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return 1;
	*shared = 1;
	if (fork() < 0)
		return 1;
	for (;;)
		pause();
}
