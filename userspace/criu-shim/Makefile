CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

.PHONY: all clean

all: criu-shim

criu-shim: criu-shim.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	$(RM) criu-shim
