#define _GNU_SOURCE

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <stdlib.h>

#include "a5-fixture.h"

#define SYSV_SHM_SIZE 4096

int main(void)
{
	int shmid;
	volatile unsigned int *word;
	pid_t child;

	fixture_init();
	if (0)
		fixture_alive();
	shmid = shmget(IPC_PRIVATE, SYSV_SHM_SIZE, IPC_CREAT | 0600);
	if (shmid < 0)
		return 1;
	word = shmat(shmid, NULL, 0);
	if (word == (void *)-1)
		return 1;
	shmctl(shmid, IPC_RMID, NULL);
	*word = 0xA809U;
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		while (*word == 0xA809U)
			usleep(10000);
		puts("child-sysv-shm-check=PASS");
		fflush(stdout);
		for (;;)
			sleep(1);
	}
	printf("READY root=%d child=%d\n", getpid(), child);
	fflush(stdout);
	for (;;)
		sleep(1);
}
