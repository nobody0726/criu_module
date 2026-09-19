#include <unistd.h>

int main(void)
{
	for (;;) {
		pid_t child = fork();
		if (child == 0)
			_exit(0);
		if (child > 0)
			usleep(1000);
	}
}
