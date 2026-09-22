CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS += -I../../include

ARCH := $(shell uname -m)
OBJS := image_reader.o criu_wire.o criu_image_reader.o validator.o carrier.o files.o staging.o sigframe.o cleanup.o task_restore.o rst_pstree.o rst_shared.o rst_session.o rst_fork.o rst_cleanup.o
ifeq ($(shell uname -s),Linux)
ifneq (,$(filter aarch64 arm64,$(ARCH)))
OBJS += bootstrap.o
endif
endif

all: libmini_restore.a mini-restore

libmini_restore.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

mini-restore: main.o libmini_restore.a
	$(CC) $(CFLAGS) -o $@ main.o libmini_restore.a

clean:
	rm -f $(OBJS) main.o libmini_restore.a mini-restore

.PHONY: all clean
