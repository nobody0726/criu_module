#!/usr/bin/env bash
# Build a debug-instrumented kernel for CRIU kernel-module development.
#
# The config below is not arbitrary. DEBUG_VM + PROVE_LOCKING +
# DEBUG_ATOMIC_SLEEP are this project's core safety net: we are writing mm
# code, and these three turn "happened not to crash" into a loud warning.
#
# Usage: ./scripts/build-kernel.sh [version]   (default 5.10.29)
set -euo pipefail

VERSION="${1:-5.10.29}"
SERIES="v${VERSION%%.*}.x"
KROOT="${KROOT:-$HOME/kernels}"
KDIR="$KROOT/linux-$VERSION"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$KROOT"

if [ ! -d "$KDIR" ]; then
	echo ">>> Fetching linux-$VERSION"
	curl -fL --retry 3 -o "$KROOT/linux-$VERSION.tar.xz" \
		"https://cdn.kernel.org/pub/linux/kernel/$SERIES/linux-$VERSION.tar.xz"
	tar -C "$KROOT" -xf "$KROOT/linux-$VERSION.tar.xz"
fi

cd "$KDIR"

echo ">>> Base config"
make -s defconfig

echo ">>> Applying debug + C/R config"
enable() { ./scripts/config --enable "$1"; }
set_val() { ./scripts/config --set-val "$1" "$2"; }
disable() { ./scripts/config --disable "$1"; }

# Visibility gate for several options below.
enable CONFIG_EXPERT

# --- module support ---
enable CONFIG_MODULES
enable CONFIG_MODULE_UNLOAD
enable CONFIG_MODULE_FORCE_UNLOAD

# --- symbols and debug info: readable oopses, working gdb ---
enable CONFIG_KALLSYMS
enable CONFIG_KALLSYMS_ALL
enable CONFIG_DEBUG_INFO
enable CONFIG_DEBUG_INFO_DWARF4
disable CONFIG_DEBUG_INFO_REDUCED
enable CONFIG_GDB_SCRIPTS
enable CONFIG_FRAME_POINTER

# --- the core safety net ---
enable CONFIG_DEBUG_KERNEL
enable CONFIG_DEBUG_VM		# internal consistency asserts for mm/
enable CONFIG_DEBUG_VM_RB	# VMA rbtree consistency (5.10 still has the rbtree)
enable CONFIG_DEBUG_VM_PGFLAGS
enable CONFIG_PROVE_LOCKING	# lockdep: catches mmap_lock ordering mistakes
enable CONFIG_DEBUG_ATOMIC_SLEEP # catches sleeping while holding a spinlock
enable CONFIG_DEBUG_LIST	# we walk VMA lists constantly
enable CONFIG_DEBUG_SPINLOCK
enable CONFIG_DEBUG_MUTEXES
enable CONFIG_STACKTRACE
enable CONFIG_SCHED_DEBUG
enable CONFIG_PANIC_ON_OOPS	# fail loudly in CI instead of limping on

# KASAN costs ~3x runtime. Worth it; set KASAN=0 to skip on slow hosts.
if [ "${KASAN:-1}" = "1" ]; then
	enable CONFIG_KASAN
	enable CONFIG_KASAN_GENERIC
	enable CONFIG_KASAN_INLINE
fi

# --- interfaces the module and CRIU need ---
enable CONFIG_DEBUG_FS		# A1's probe interface
enable CONFIG_CHECKPOINT_RESTORE # /proc/*/map_files, kcmp(), etc.
enable CONFIG_MEM_SOFT_DIRTY	# incremental dump
enable CONFIG_PROC_FS
enable CONFIG_PROC_PAGE_MONITOR	# /proc/*/pagemap
enable CONFIG_FUTEX		# restore-stage barriers
enable CONFIG_EVENTFD
enable CONFIG_EPOLL
enable CONFIG_SIGNALFD
enable CONFIG_TIMERFD
enable CONFIG_FANOTIFY
enable CONFIG_INOTIFY_USER
enable CONFIG_UNIX		# unix domain sockets (A5)
enable CONFIG_INET
enable CONFIG_INET_TCP_DIAG	# TCP_REPAIR needs sock_diag
enable CONFIG_INET_DIAG
enable CONFIG_PACKET_DIAG
enable CONFIG_UNIX_DIAG
enable CONFIG_NETLINK_DIAG
enable CONFIG_SYSVIPC		# SysV shm (A8)
enable CONFIG_POSIX_MQUEUE
enable CONFIG_NAMESPACES
enable CONFIG_PID_NS
enable CONFIG_NET_NS
enable CONFIG_UTS_NS
enable CONFIG_IPC_NS
enable CONFIG_USER_NS
enable CONFIG_CGROUPS
enable CONFIG_FREEZER
enable CONFIG_CGROUP_FREEZER
enable CONFIG_TTY
enable CONFIG_UNIX98_PTYS	# --shell-job tests
enable CONFIG_BLK_DEV_INITRD

# --- boot in QEMU without a disk ---
enable CONFIG_VIRTIO
enable CONFIG_VIRTIO_PCI
enable CONFIG_VIRTIO_BLK
enable CONFIG_VIRTIO_NET
enable CONFIG_VIRTIO_CONSOLE
enable CONFIG_NET_9P
enable CONFIG_NET_9P_VIRTIO
enable CONFIG_9P_FS		# host directory passthrough
enable CONFIG_SERIAL_8250
enable CONFIG_SERIAL_8250_CONSOLE
enable CONFIG_TMPFS
enable CONFIG_DEVTMPFS
enable CONFIG_DEVTMPFS_MOUNT

# Signing would force us to re-sign the module on every rebuild.
disable CONFIG_MODULE_SIG_ALL
disable CONFIG_MODULE_SIG_FORCE
disable CONFIG_SYSTEM_TRUSTED_KEYRING
set_val CONFIG_SYSTEM_TRUSTED_KEYS '""'
disable CONFIG_DEBUG_INFO_BTF

make -s olddefconfig

echo ">>> Building with $JOBS jobs (first build: 20-40 min)"
make -j"$JOBS" bzImage modules

echo ">>> Building initramfs (fallback for run-qemu.sh when virtme-ng is absent)"
IRD="$KROOT/initramfs-$VERSION"
rm -rf "$IRD"
mkdir -p "$IRD"/{bin,sbin,proc,sys,dev,tmp,mnt,root}
cp "$(command -v busybox)" "$IRD/bin/busybox"
( cd "$IRD/bin" && for a in sh ls cat mount umount insmod rmmod dmesg \
	sleep kill ps grep mkdir echo cp mv rm chmod dd; do ln -sf busybox "$a"; done )
cat > "$IRD/init" <<'INIT'
#!/bin/sh
mount -t proc  proc  /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
mkdir -p /mnt/host
# 9p passthrough of the project directory, mounted by run-qemu.sh's -virtfs
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host 2>/dev/null
echo "=== guest up: $(uname -r) ==="
if [ -x /mnt/host/guest-script.sh ]; then
	/mnt/host/guest-script.sh
	echo "=== guest script done, powering off ==="
	poweroff -f
else
	exec /bin/sh
fi
INIT
chmod +x "$IRD/init"
( cd "$IRD" && find . | cpio -o -H newc --quiet | gzip -9 \
	> "$KROOT/initramfs-$VERSION.cpio.gz" )

echo
echo ">>> Done."
echo "    kernel:    $KDIR/arch/x86/boot/bzImage"
echo "    vmlinux:   $KDIR/vmlinux   (feed this to gdb)"
echo "    initramfs: $KROOT/initramfs-$VERSION.cpio.gz"
echo
echo "    Build the module with:"
echo "      make -C kernel_module KDIR=$KDIR"
