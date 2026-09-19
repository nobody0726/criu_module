CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS += -I../../include

OBJS := image_reader.o validator.o carrier.o files.o staging.o sigframe.o

all: libmini_restore.a

libmini_restore.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f $(OBJS) libmini_restore.a

.PHONY: all clean
