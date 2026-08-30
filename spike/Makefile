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
ifeq ($(PROBE),4.1)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_1
endif
ifeq ($(PROBE),4.2)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_2
endif
ifeq ($(PROBE),4.3)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_3
endif
ifeq ($(PROBE),4.4a)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_4A
endif
ifeq ($(PROBE),4.4b)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_4B
endif
ifeq ($(PROBE),4.5a)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_5A
endif
ifeq ($(PROBE),4.5b)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_5B
endif
ifeq ($(PROBE),4.6)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_6
endif
ifeq ($(PROBE),4.7)
ccflags-y += -DCRIU_SPIKE_COMPILE_4_7
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
