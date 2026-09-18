#define _GNU_SOURCE
#include <sys/socket.h>
#include "a5-fixture.h"

int main(void)
{
	int s[2], half[2], duplicate;
	char buf[64];
	fixture_init();
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s) ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, half))
		return 1;
	duplicate = dup(s[0]);
	if (duplicate < 0 || write(s[0], "prefixleft-data", 15) != 15 ||
	    read(s[1], buf, 6) != 6 || write(s[1], "right-data", 10) != 10 ||
	    write(half[0], "end", 3) != 3 || shutdown(half[0], SHUT_WR))
		return 1;
	printf("pid=%d left_fd=%d right_fd=%d\n", getpid(), s[0], s[1]);
	fflush(stdout);
	fixture_command();
	if (read(duplicate, buf, sizeof(buf)) != 10 || memcmp(buf, "right-data", 10) ||
	    read(s[1], buf, sizeof(buf)) != 9 || memcmp(buf, "left-data", 9))
		return 2;
	if (write(s[1], "reply", 5) != 5 || read(s[0], buf, sizeof(buf)) != 5 ||
	    memcmp(buf, "reply", 5) || write(s[0], "back", 4) != 4 ||
	    read(s[1], buf, sizeof(buf)) != 4 || memcmp(buf, "back", 4))
		return 3;
	if (read(half[1], buf, sizeof(buf)) != 3 || memcmp(buf, "end", 3) ||
	    read(half[1], buf, sizeof(buf)) != 0)
		return 4;
	errno = 0;
	if (send(half[0], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return 5;
	if (write(half[1], "ok", 2) != 2 || read(half[0], buf, sizeof(buf)) != 2 ||
	    memcmp(buf, "ok", 2))
		return 6;
	puts("unix-check=PASS bidirectional=PASS unread=PASS shutdown=PASS dup=PASS");
	fflush(stdout);
	fixture_alive();
}
