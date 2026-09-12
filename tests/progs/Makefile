CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Werror
LDFLAGS = -static
PROGRAMS = a1-layout a1-check many-vmas busy-counter multithread-counter stopped-counter minimal

.PHONY: all clean
all: $(PROGRAMS)

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	$(RM) $(PROGRAMS)
