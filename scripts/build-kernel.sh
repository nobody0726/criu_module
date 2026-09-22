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
KERNEL_TARGETS="${KERNEL_TARGETS:-Image modules}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "$KROOT"

if [ ! -d "$KDIR" ]; then
	echo ">>> Fetching linux-$VERSION"
	curl -fL --retry 3 -o "$KROOT/linux-$VERSION.tar.xz" \
		"https://cdn.kernel.org/pub/linux/kernel/$SERIES/linux-$VERSION.tar.xz"
	tar -C "$KROOT" -xf "$KROOT/linux-$VERSION.tar.xz"
fi

cd "$KDIR"

if [ "$VERSION" = "5.10.29" ]; then
	"$PROJECT_DIR/scripts/apply-kernel-patches.sh" "$KDIR"
fi

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
disable CONFIG_ARM64_PTR_AUTH # CRIU 4.2.1 cannot query PAC on 5.10.29
enable CONFIG_DEBUG_FS		# A1's probe interface
enable CONFIG_CHECKPOINT_RESTORE # /proc/*/map_files, kcmp(), etc.
enable CONFIG_BINFMT_MISC         # ZDTM's binfmt_misc mount preflight
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
enable CONFIG_VETH		# CRIU kerndat creates a veth pair during startup
enable CONFIG_NETFILTER
enable CONFIG_NF_TABLES
enable CONFIG_NF_TABLES_INET
# Linux 5.10 implements concatenated sets in NF_TABLES itself; there is no
# CONFIG_NFT_CONCAT symbol.  CRIU's probe only creates an inet concatenated
# set, so NF_TABLES_INET is the required family support.
enable CONFIG_UTS_NS
enable CONFIG_IPC_NS
enable CONFIG_USER_NS
enable CONFIG_CGROUPS
enable CONFIG_FREEZER
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
enable CONFIG_SERIAL_AMBA_PL011
enable CONFIG_SERIAL_AMBA_PL011_CONSOLE
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
make ARCH=arm64 -j"$JOBS" $KERNEL_TARGETS

echo ">>> Building initramfs (fallback for run-qemu.sh when virtme-ng is absent)"
IRD="$KROOT/initramfs-$VERSION"
rm -rf "$IRD"
mkdir -p "$IRD"/{bin,sbin,proc,sys,dev,dev/pts,proc/sys/fs/binfmt_misc,tmp,mnt,root}
# The Lima guest commonly uses a group-writable umask.  CRIU records the
# target's root directory mode and restore validates it, so keep the initramfs
# root at the conventional 0755 regardless of the build user's umask.
chmod 755 "$IRD"
cp "$(command -v busybox)" "$IRD/bin/busybox"

( cd "$IRD/bin" && for a in sh ls cat mount umount mountpoint insmod rmmod dmesg ip setsid \
	sleep kill ps grep mkdir echo true false cp mv rm chmod dd id tail uname poweroff; do ln -sf busybox "$a"; done )
# ZDTM invokes rm from a restricted /bin-first PATH and needs GNU's
# --one-file-system option during its cleanout targets.  The binary is copied
# after the BusyBox symlink is removed so the archive contains GNU rm itself.
rm -f "$IRD/bin/rm"
cp /usr/bin/rm "$IRD/bin/rm"
cat > "$IRD/init" <<'INIT'
#!/bin/sh
mount -t proc  proc  /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
mkdir -p /mnt/host
# 9p passthrough of the project directory, mounted by run-qemu.sh's -virtfs
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host 2>/dev/null
# Read-only Lima runtime mounts used by A3's real CRIU/crit gate.  The
# fallback initramfs remains small while dynamically linked ARM64 tools are
# resolved from the Lima build guest.  Missing mounts are tolerated so the
# earlier kernel/module gates keep working in reduced environments.
mkdir -p /usr /lib
mount -t 9p -o trans=virtio,version=9p2000.L limausr /usr 2>/dev/null
mount -t 9p -o trans=virtio,version=9p2000.L limalib /lib 2>/dev/null
# CRIU's TCP_REPAIR capability probe binds 127.0.0.1.  The minimal guest has
# no distro init system, so bring up loopback explicitly before test scripts.
/bin/ip link set lo up 2>/dev/null || true
echo "=== guest up: $(uname -r) ==="
if [ -x /mnt/host/guest-script.sh ]; then
	echo "=== running guest script ==="
	/mnt/host/guest-script.sh
	rc=$?
	echo "=== guest script exit: $rc; powering off ==="
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
echo "    kernel:    $KDIR/arch/arm64/boot/Image"
echo "    vmlinux:   $KDIR/vmlinux   (feed this to gdb)"
echo "    initramfs: $KROOT/initramfs-$VERSION.cpio.gz"
echo
echo "    Build the module with:"
echo "      make -C kernel_module KDIR=$KDIR"
