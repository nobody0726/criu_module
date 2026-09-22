CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Werror
LDFLAGS = -static
PROGRAMS = a1-layout a1-check many-vmas busy-counter multithread-counter threads stopped-counter minimal b1-minimal fds-dup fds-pipe fds-unix-stream fds-unsupported shared-fdt shared-file-offset a8-cross-fd shm-anon shm-posix shm-sysv sig-handlers sig-pending timers tree-simple tree-session tree-pgid tree-invalid-leader tree-fork-race tree-shared-unsupported

.PHONY: all clean
all: $(PROGRAMS)

all: b1-carrier-probe
b1-carrier-probe: b1-carrier-probe.c ../../userspace/mini-restore/libmini_restore.a
	$(CC) $(CFLAGS) -I../../include $< ../../userspace/mini-restore/libmini_restore.a $(LDFLAGS) -o $@

fds-dup fds-pipe fds-unix-stream fds-unsupported: a5-fixture.h
shared-fdt shared-file-offset: a5-fixture.h
a8-cross-fd shm-anon shm-posix shm-sysv: a5-fixture.h

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	$(RM) $(PROGRAMS)
