#include <unistd.h>

int main(void)
{
	pid_t child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(0);
	for (;;)
		pause();
}
