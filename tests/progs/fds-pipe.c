#define _GNU_SOURCE
#include <fcntl.h>
#include "a5-fixture.h"

int main(void)
{
	int p[2], empty[2], closed[2], bulk[2], duplicate;
	char buf[32];
	unsigned char bytes[4096], restored[4096];
	unsigned i;
	fixture_init();
	if (pipe2(p, O_NONBLOCK) || pipe2(empty, O_NONBLOCK) ||
	    pipe2(closed, O_NONBLOCK) || pipe2(bulk, O_NONBLOCK))
		return 1;
	for (i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)i;
	if (write(bulk[1], bytes, sizeof(bytes)) != sizeof(bytes)) return 1;
	duplicate = dup(p[0]);
	if (duplicate < 0 || write(p[1], "prefixpipe-data", 15) != 15 ||
	    read(p[0], buf, 6) != 6 || write(closed[1], "end", 3) != 3 ||
	    close(closed[1]))
		return 1;
	printf("pid=%d read_fd=%d write_fd=%d\n", getpid(), p[0], p[1]);
	fflush(stdout);
	fixture_command();
	if (read(bulk[0], restored, sizeof(restored)) != sizeof(restored) ||
	    memcmp(restored, bytes, sizeof(bytes)))
		return 7;
	if (read(duplicate, buf, sizeof(buf)) != 9 ||
	    memcmp(buf, "pipe-data", 9))
		return 2;
	errno = 0;
	if (read(p[0], buf, sizeof(buf)) != -1 || errno != EAGAIN)
		return 3;
	if (write(p[1], "new", 3) != 3 ||
	    read(p[0], buf, sizeof(buf)) != 3 || memcmp(buf, "new", 3) ||
	    close(p[1]) || read(duplicate, buf, sizeof(buf)) != 0)
		return 4;
	errno = 0;
	if (read(empty[0], buf, sizeof(buf)) != -1 || errno != EAGAIN ||
	    close(empty[1]) || read(empty[0], buf, sizeof(buf)) != 0)
		return 5;
	if (read(closed[0], buf, sizeof(buf)) != 3 || memcmp(buf, "end", 3) ||
	    read(closed[0], buf, sizeof(buf)) != 0)
		return 6;
	puts("pipe-check=PASS unread=pipe-data dup=PASS empty=PASS eof=PASS");
	fflush(stdout);
	fixture_alive();
}
