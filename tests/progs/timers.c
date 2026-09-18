#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void timer_handler(int signo)
{
	(void)signo;
}

int main(void)
{
	struct sigaction action;
	struct sigevent event;
	struct itimerval interval;
	timer_t timer;
	struct itimerspec spec;

	sigemptyset(&action.sa_mask);
	action.sa_handler = timer_handler;
	action.sa_flags = 0;
	action.sa_restorer = 0;
	if (sigaction(SIGALRM, &action, 0) < 0)
		return 1;
	interval.it_interval.tv_sec = 1;
	interval.it_interval.tv_usec = 0;
	interval.it_value.tv_sec = 1;
	interval.it_value.tv_usec = 0;
	if (setitimer(ITIMER_REAL, &interval, 0) < 0)
		return 1;
	event.sigev_notify = SIGEV_SIGNAL;
	event.sigev_signo = SIGRTMIN;
	event.sigev_value.sival_ptr = (void *)0xA6;
	if (timer_create(CLOCK_MONOTONIC, &event, &timer) < 0)
		return 1;
	spec.it_interval.tv_sec = 2;
	spec.it_interval.tv_nsec = 0;
	spec.it_value.tv_sec = 1;
	spec.it_value.tv_nsec = 0;
	if (timer_settime(timer, 0, &spec, 0) < 0)
		return 1;
	for (;;)
		pause();
}
