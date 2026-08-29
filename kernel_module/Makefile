obj-m += criu_probe.o

KDIR ?= $(HOME)/kernels/linux-5.10.29
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
