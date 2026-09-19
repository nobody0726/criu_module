#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "a5-fixture.h"

#define SHM_SIZE 4096

int main(void)
{
	char name[64];
	volatile uint32_t *word;
	pid_t child;
	int fd;
	char command;

	fixture_init();
	snprintf(name, sizeof(name), "/criu-module-a8-%d", getpid());
	fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
	if (fd < 0)
		return 1;
	shm_unlink(name);
	if (ftruncate(fd, SHM_SIZE))
		return 1;
	word = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (word == MAP_FAILED)
		return 1;
	*word = 0xA806U;
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		while (*word != 0xA807U)
			usleep(10000);
		*word = 0xA808U;
		puts("child-posix-shm-check=PASS");
		fflush(stdout);
		for (;;)
			sleep(1);
	}
	printf("READY root=%d child=%d\n", getpid(), child);
	fflush(stdout);
	while (read(STDIN_FILENO, &command, 1) != 1)
		usleep(10000);
	if (command != 'x')
		return 2;
	*word = 0xA807U;
	while (*word != 0xA808U)
		usleep(10000);
	puts("posix-shm-check=PASS");
	fflush(stdout);
	fixture_alive();
}
