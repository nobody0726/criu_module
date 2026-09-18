#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	sigset_t blocked;
	union sigval value;
	pid_t pid = getpid();
	pid_t tid = (pid_t)syscall(SYS_gettid);

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
	for (;;)
		pause();
}
