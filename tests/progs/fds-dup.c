#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	int independent, shared, duplicate;
	struct sigaction action;
	int signo;

	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	for (signo = 1; signo < NSIG; signo++)
		if (signo != SIGKILL && signo != SIGSTOP)
			sigaction(signo, &action, NULL);

	independent = open("/tmp/a5-shared", O_RDWR | O_CREAT | O_TRUNC, 0600);
	shared = open("/tmp/a5-shared", O_RDWR);
	duplicate = dup(shared);
	if (independent < 0 || shared < 0 || duplicate < 0)
		return 1;
	if (lseek(independent, 3, SEEK_SET) != 3 ||
		lseek(shared, 7, SEEK_SET) != 7 ||
		lseek(duplicate, 0, SEEK_CUR) != 7)
		return 2;
	printf("pid=%d independent=%d shared=%d duplicate=%d\n",
		getpid(), independent, shared, duplicate);
	fflush(stdout);
	for (;;) {
		if (lseek(shared, 5, SEEK_SET) != 5 ||
			lseek(duplicate, 0, SEEK_CUR) != 5)
			return 3;
		if (lseek(independent, 3, SEEK_SET) != 3)
			return 4;
		sleep(1);
	}
}
