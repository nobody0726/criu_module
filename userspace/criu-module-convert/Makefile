CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS += -I../../include

OBJS := main.o snapshot_reader.o criu_model.o image_writer.o

all: criu-module-convert
criu-module-convert: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)
clean:
	rm -f $(OBJS) criu-module-convert
.PHONY: all clean
