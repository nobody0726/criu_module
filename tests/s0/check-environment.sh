#!/bin/sh

set -u

home_dir=${HOME:-}
KDIR=${KDIR:-${home_dir}/kernels/linux-5.10.29}
config_file=${KDIR}/.config
mismatch=0

env_mismatch()
{
	printf 'ENV-MISMATCH: %s\n' "$1" >&2
	mismatch=1
}

arch=$(uname -m 2>/dev/null || printf 'unknown')
if [ "$arch" != aarch64 ]; then
	env_mismatch "uname -m is ${arch}, expected aarch64"
fi

kernel_release=unknown
if [ ! -d "$KDIR" ]; then
	env_mismatch "KDIR does not exist: ${KDIR}"
else
	kernel_release=$(make -s -C "$KDIR" kernelrelease 2>/dev/null | awk 'NF { value = $0 } END { print value }')
	if [ -z "$kernel_release" ]; then
		env_mismatch "could not read kernel release from ${KDIR}"
	elif [ "$kernel_release" != 5.10.29 ]; then
		env_mismatch "kernel release is ${kernel_release}, expected 5.10.29"
	fi
fi

config_value()
{
	awk -v key="$1" '
		$0 == key "=y" { print "y"; found = 1; exit }
		$0 == key "=m" { print "m"; found = 1; exit }
		$0 ~ "^" key "=" {
			print substr($0, length(key) + 2)
			found = 1
			exit
		}
		$0 == "# " key " is not set" { print "unset"; found = 1; exit }
		END { if (!found) print "missing" }
	' "$config_file"
}

if [ ! -f "$config_file" ]; then
	env_mismatch "kernel config does not exist: ${config_file}"
else
	for config_name in \
		CONFIG_MODULES \
		CONFIG_DEBUG_VM \
		CONFIG_DEBUG_VM_RB \
		CONFIG_PROVE_LOCKING \
		CONFIG_DEBUG_ATOMIC_SLEEP \
		CONFIG_DEBUG_LIST \
		CONFIG_DEBUG_FS \
		CONFIG_KASAN \
		CONFIG_9P_FS
	do
		config_status=$(config_value "$config_name")
		if [ "$config_status" != y ]; then
			env_mismatch "${config_name} is ${config_status}, expected y"
		fi
	done

	ptr_auth_status=$(config_value CONFIG_ARM64_PTR_AUTH)
	case "$ptr_auth_status" in
		unset)
			;;
		*)
			env_mismatch "CONFIG_ARM64_PTR_AUTH is ${ptr_auth_status}, expected unset"
			;;
	esac
fi

if [ "$mismatch" -ne 0 ]; then
	exit 1
fi

hash_input=$(awk '
	/^CONFIG_[A-Za-z0-9_]+=/{ print }
	/^# CONFIG_[A-Za-z0-9_]+ is not set$/{ print }
' "$config_file" | LC_ALL=C sort)
if command -v sha256sum >/dev/null 2>&1; then
	config_hash=$(printf '%s\n' "$hash_input" | sha256sum | awk '{ print $1 }')
elif command -v shasum >/dev/null 2>&1; then
	config_hash=$(printf '%s\n' "$hash_input" | shasum -a 256 | awk '{ print $1 }')
else
	env_mismatch 'neither sha256sum nor shasum is available'
	exit 1
fi

printf 'KERNEL_RELEASE=%s\n' "$kernel_release"
printf 'ARCH=%s\n' "$arch"
printf 'KDIR=%s\n' "$KDIR"
printf 'CONFIG_HASH=%s\n' "$config_hash"
