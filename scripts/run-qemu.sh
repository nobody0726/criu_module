#!/usr/bin/env bash
# Boot the debug kernel in QEMU with the project directory passed through via
# 9p. Never insmod on your development machine: one bad vma->vm_next
# dereference is an unrecoverable oops, and with bad luck it corrupts the fs.
#
#   ./scripts/run-qemu.sh                      # interactive shell in guest
#   ./scripts/run-qemu.sh --gdb                # wait for gdb on :1234
#   ./scripts/run-qemu.sh --ci --script tests/ci-smoke.sh
set -euo pipefail

VERSION="${KVERSION:-5.10.29}"
KROOT="${KROOT:-$HOME/kernels}"
KDIR="$KROOT/linux-$VERSION"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The Lima /Users mount can expose host-native binaries to nested QEMU even
# after an ARM64 build in Lima.  Stage the complete project on Lima-local
# storage so the guest sees the Linux ELF artifacts produced by that build.
QEMU_PROJECT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/criu-module-qemu.XXXXXX")"
cp -a "$PROJECT_DIR"/. "$QEMU_PROJECT_DIR"/
# A worktree need not duplicate the untracked upstream CRIU checkout. Stage
# only its runtime binary and Python image decoder when explicitly supplied.
if [ -n "${CRIU_SOURCE:-}" ]; then
	test -x "$CRIU_SOURCE/criu/criu"
	mkdir -p "$QEMU_PROJECT_DIR/criu/criu"
	cp "$CRIU_SOURCE/criu/criu" "$QEMU_PROJECT_DIR/criu/criu/criu"
	cp -a "$CRIU_SOURCE/crit" "$CRIU_SOURCE/lib" "$QEMU_PROJECT_DIR/criu/"
fi
if [ "$(uname -s)" = Linux ] && [ -f "$QEMU_PROJECT_DIR/userspace/Makefile" ]; then
	# Rebuild executable artifacts inside the Lima-local staging tree.  A
	# source tree mounted from macOS may contain Mach-O files even when the
	# same path looked like an ELF during the Lima build.
	make -C "$QEMU_PROJECT_DIR/userspace" clean all >/dev/null
fi
if [ "$(uname -s)" = Linux ] && [ -f "$QEMU_PROJECT_DIR/tests/progs/Makefile" ]; then
	# The guest gates execute test fixtures from /mnt/host/tests/progs.
	# Build them in the same Lima-local staging tree that is passed to QEMU
	# so a fresh worktree does not depend on untracked host-side binaries.
	make -C "$QEMU_PROJECT_DIR/tests/progs" clean all >/dev/null
	find "$QEMU_PROJECT_DIR/tests/progs" -maxdepth 1 -type f -perm /111 \
		-exec chmod 0755 {} +
fi

GDB=0
CI=0
SCRIPT=""
MEM="${MEM:-2G}"
CPUS="${CPUS:-2}"
STATUS_FILE="$QEMU_PROJECT_DIR/.qemu-guest.status"
CPU_MODEL="${QEMU_CPU:-cortex-a72}"

while [ $# -gt 0 ]; do
	case "$1" in
	--gdb)    GDB=1; shift ;;
	--ci)     CI=1; shift ;;
	--script) SCRIPT="$2"; shift 2 ;;
	--mem)    MEM="$2"; shift 2 ;;
	--cpus)   CPUS="$2"; shift 2 ;;
	*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
done

IMAGE="$KDIR/arch/arm64/boot/Image"
INITRAMFS="$KROOT/initramfs-$VERSION.cpio.gz"

for f in "$IMAGE" "$INITRAMFS"; do
	if [ ! -f "$f" ]; then
		echo "missing $f -- run ./scripts/build-kernel.sh $VERSION first" >&2
		exit 1
	fi
done

# The guest's /init runs /mnt/host/guest-script.sh if present. Stage the
# requested script there, and clean it up on exit so an interactive run later
# does not silently execute a stale script.
GUEST_ENTRY="$QEMU_PROJECT_DIR/guest-script.sh"
cleanup() { rm -f "$GUEST_ENTRY" "$STATUS_FILE"; rm -rf "$QEMU_PROJECT_DIR"; }
trap cleanup EXIT
rm -f "$GUEST_ENTRY" "$STATUS_FILE"

if [ -n "$SCRIPT" ]; then
	if [ ! -f "$PROJECT_DIR/$SCRIPT" ]; then
		echo "no such script: $SCRIPT" >&2
		exit 1
	fi
	cat > "$GUEST_ENTRY" <<EOF
#!/bin/sh
cd /mnt/host
/bin/sh "$SCRIPT"
rc=\$?
echo "\$rc" > /mnt/host/.qemu-guest.status
exit "\$rc"
EOF
	chmod +x "$GUEST_ENTRY"
fi

QEMU_ARGS=(
	-kernel "$IMAGE"
	-M virt
	-initrd "$INITRAMFS"
	-m "$MEM"
	-smp "$CPUS"
	-nographic
	-no-reboot
	# 9p passthrough: guest sees this repo at /mnt/host, no image rebuild
	# needed between edits.
	-virtfs "local,path=$QEMU_PROJECT_DIR,mount_tag=hostshare,security_model=none,id=hostshare"
	# The fallback initramfs is intentionally tiny.  These read-only 9p mounts
	# expose the Lima build guest's ARM64 runtime so the guest can run the
	# dynamically linked CRIU and Python/crit without copying host binaries into
	# the target rootfs.  Kernel-module and target-process execution remain in
	# the 5.10.29 guest.
	-virtfs "local,path=/usr,mount_tag=limausr,security_model=none,id=limausr,readonly=on"
	-virtfs "local,path=/lib,mount_tag=limalib,security_model=none,id=limalib,readonly=on"
	-append "console=ttyAMA0 panic=1 oops=panic nokaslr loglevel=7"
)

if [ "$CI" = 1 ]; then
	QEMU_ARGS[-1]="${QEMU_ARGS[-1]} quiet_but_not_really"
fi

# KVM is unavailable on GitHub's standard runners (no nested virt), so CI
# falls back to TCG. Locally, use KVM when the host offers it.
if [ -w /dev/kvm ] && [ "$CI" = 0 ]; then
	QEMU_ARGS+=(-enable-kvm -cpu host)
	echo ">>> KVM enabled"
else
	QEMU_ARGS+=(-accel tcg -cpu "$CPU_MODEL")
	echo ">>> TCG mode (no KVM) -- expect ~10x slowdown"
fi

if [ "$GDB" = 1 ]; then
	QEMU_ARGS+=(-s -S)
	cat <<EOF

>>> QEMU is stopped, waiting for gdb on tcp::1234.
    In another terminal:

      gdb $KDIR/vmlinux
      (gdb) target remote :1234
      (gdb) lx-symbols $PROJECT_DIR/kernel_module
      (gdb) break criu_dump_memory
      (gdb) continue

    lx-symbols needs CONFIG_GDB_SCRIPTS (build-kernel.sh enables it) and
    loads your module's symbols so breakpoints on module functions resolve.

EOF
fi

# nokaslr above is what makes gdb addresses stable across boots.
echo ">>> Booting linux-$VERSION"
if qemu-system-aarch64 "${QEMU_ARGS[@]}"; then
	qemu_rc=0
else
	qemu_rc=$?
fi

if [ -f "$STATUS_FILE" ]; then
	guest_rc=$(tr -d '[:space:]' < "$STATUS_FILE")
	case "$guest_rc" in
	[0-9]*) exit "$guest_rc" ;;
	esac
fi

if [ "$qemu_rc" -eq 0 ] && [ -n "$SCRIPT" ]; then
	echo "guest did not report a completion status" >&2
	exit 125
fi
exit "$qemu_rc"
