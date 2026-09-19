#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t seen;
static volatile sig_atomic_t rt_value_ok;

static void pending_handler(int signo, siginfo_t *info, void *context)
{
	(void)context;
	if (signo == SIGUSR1)
		seen |= 1;
	else if (signo == SIGUSR2)
		seen |= 2;
	else if (signo == SIGRTMIN) {
		seen |= 4;
		rt_value_ok = info && info->si_value.sival_int == 0xA6;
	}
}

int main(void)
{
	struct sigaction action;
	sigset_t blocked;
	union sigval value;
	pid_t pid = getpid();
	pid_t tid = (pid_t)syscall(SYS_gettid);

	sigemptyset(&action.sa_mask);
	action.sa_sigaction = pending_handler;
	action.sa_flags = SA_SIGINFO;
	action.sa_restorer = 0;
	if (sigaction(SIGUSR1, &action, 0) < 0 ||
	    sigaction(SIGUSR2, &action, 0) < 0 ||
	    sigaction(SIGRTMIN, &action, 0) < 0)
		return 1;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	sigaddset(&blocked, SIGRTMIN);
	if (sigprocmask(SIG_BLOCK, &blocked, 0) < 0)
		return 1;
	if (raise(SIGUSR1) != 0)
		return 1;
	value.sival_int = 0xA6;
	if (sigqueue(pid, SIGRTMIN, value) < 0)
		return 1;
	if (syscall(SYS_tgkill, pid, tid, SIGUSR2) < 0 && errno != 0)
		return 1;
	printf("pid=%ld\n", (long)pid);
	fflush(stdout);
	sleep(2);
	if (sigprocmask(SIG_UNBLOCK, &blocked, 0) < 0)
		return 1;
	if (seen != 7 || !rt_value_ok)
		return 1;
	puts("pending=PASS");
	fflush(stdout);
	for (;;)
		pause();
}
