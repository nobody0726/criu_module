obj-m += criu_spike.o

KDIR ?= $(HOME)/kernels/linux-5.10.29
PWD := $(shell pwd)
PROBE ?= 2.2

ifeq ($(shell uname -s),Darwin)
$(error Linux kernel modules must be built in the Linux 5.10.29 guest, not on macOS)
endif

ifeq ($(wildcard $(KDIR)/Makefile),)
$(error KDIR does not contain a kernel Makefile: $(KDIR))
endif

# Keep the probe ID as text so IDs such as 4.4a remain lossless.
ccflags-y += -DCRIU_SPIKE_PROBE=\"$(PROBE)\"
ifeq ($(PROBE),1.1)
ccflags-y += -DCRIU_SPIKE_COMPILE_1_1
endif
ifeq ($(PROBE),2.1)
ccflags-y += -DCRIU_SPIKE_COMPILE_2_1
endif
ifeq ($(PROBE),3.1)
ccflags-y += -DCRIU_SPIKE_COMPILE_3_1
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
