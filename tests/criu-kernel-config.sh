#!/usr/bin/env bash
set -euo pipefail

KDIR=${KDIR:-"$HOME/kernels/linux-5.10.29"}
config="$KDIR/.config"

[ -r "$config" ] || {
	echo "CRIU_KERNEL_CONFIG: FAIL: missing $config" >&2
	exit 1
}

# The fallback QEMU initramfs deliberately has no kernel-module tree.  CRIU's
# kerndat probes must therefore have these networking features built in.
for key in \
	CONFIG_NET_NS \
	CONFIG_CHECKPOINT_RESTORE \
	CONFIG_VETH \
	CONFIG_NF_TABLES \
	CONFIG_NF_TABLES_INET; do
	grep -qx "${key}=y" "$config" || {
		echo "CRIU_KERNEL_CONFIG: FAIL: ${key}=y is required" >&2
		exit 1
	}
done

echo "CRIU_KERNEL_CONFIG: PASS"
