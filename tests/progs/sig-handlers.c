#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t handled;

static void info_handler(int signo, siginfo_t *info, void *context)
{
	(void)info;
	(void)context;
	handled = signo;
}

static void plain_handler(int signo)
{
	handled = signo;
}

int main(void)
{
	struct sigaction action;

	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGUSR2);
	action.sa_sigaction = info_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	action.sa_restorer = 0;
	if (sigaction(SIGUSR1, &action, 0) < 0)
		return 1;
	sigemptyset(&action.sa_mask);
	action.sa_handler = plain_handler;
	action.sa_flags = SA_RESTART;
	action.sa_restorer = 0;
	if (sigaction(SIGUSR2, &action, 0) < 0)
		return 1;
	if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
		return 1;
	printf("pid=%ld\n", (long)getpid());
	fflush(stdout);
	for (;;) {
		pause();
		if (handled == SIGUSR1) {
			puts("handler=PASS");
			fflush(stdout);
			handled = 0;
		}
	}
}
