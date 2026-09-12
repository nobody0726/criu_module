obj-m += criu_kernel.o
criu_kernel-y := core/main.o core/target.o checkpoint/freeze.o checkpoint/vma_walk.o checkpoint/snapshot_writer.o checkpoint/dump_task.o checkpoint/dump_files.o checkpoint/dump_mm.o checkpoint/page_scan.o checkpoint/dump.o
ccflags-y += -I$(src)/include

KDIR ?= $(HOME)/kernels/linux-5.10.29
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
