obj-m += criu_kernel.o
criu_kernel-y := core/main.o core/target.o core/objmap.o checkpoint/freeze.o checkpoint/collect_tree.o checkpoint/vma_walk.o checkpoint/snapshot_writer.o checkpoint/dump_task.o checkpoint/dump_threads.o checkpoint/dump_signals.o checkpoint/dump_timers.o checkpoint/dump_shared.o checkpoint/dump_files.o checkpoint/dump_mm.o checkpoint/page_scan.o checkpoint/dump_pstree.o checkpoint/dump.o
ccflags-y += -I$(src)/include -I$(srctree)/kernel/time

KDIR ?= $(HOME)/kernels/linux-5.10.29
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
