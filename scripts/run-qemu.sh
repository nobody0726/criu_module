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

GDB=0
CI=0
SCRIPT=""
MEM="${MEM:-2G}"
CPUS="${CPUS:-2}"
STATUS_FILE="$PROJECT_DIR/.qemu-guest.status"
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
GUEST_ENTRY="$PROJECT_DIR/guest-script.sh"
cleanup() { rm -f "$GUEST_ENTRY" "$STATUS_FILE"; }
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
	-virtfs "local,path=$PROJECT_DIR,mount_tag=hostshare,security_model=none,id=hostshare"
	-append "console=ttyAMA0 panic=1 oops=panic nokaslr loglevel=7"
)

if [ "$CI" = 1 ]; then
	QEMU_ARGS[-1]="$QEMU_ARGS[-1] quiet_but_not_really"
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

exit "$qemu_rc"
