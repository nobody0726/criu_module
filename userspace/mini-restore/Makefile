CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS += -I../../include

OBJS := image_reader.o validator.o carrier.o files.o staging.o sigframe.o cleanup.o

all: libmini_restore.a mini-restore

libmini_restore.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

mini-restore: main.o libmini_restore.a
	$(CC) $(CFLAGS) -o $@ main.o libmini_restore.a

clean:
	rm -f $(OBJS) main.o libmini_restore.a mini-restore

.PHONY: all clean
