CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=

OS := $(shell uname -s)
ifeq ($(OS),Linux)
LDFLAGS += -static
endif

.PHONY: all clean

all: known-layout

known-layout: known-layout.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f known-layout
