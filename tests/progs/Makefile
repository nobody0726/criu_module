CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Werror
LDFLAGS = -static
PROGRAMS = a1-layout a1-check many-vmas busy-counter multithread-counter threads stopped-counter minimal fds-dup fds-pipe fds-unix-stream fds-unsupported sig-handlers sig-pending timers tree-simple tree-session tree-pgid tree-invalid-leader tree-fork-race tree-shared-unsupported

.PHONY: all clean
all: $(PROGRAMS)

fds-dup fds-pipe fds-unix-stream fds-unsupported: a5-fixture.h

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	$(RM) $(PROGRAMS)
