CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=

LDFLAGS += -static

.PHONY: all clean

all: known-layout

known-layout: known-layout.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f known-layout
