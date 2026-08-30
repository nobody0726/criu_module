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
	kernelrelease_output=$(make -s -C "$KDIR" kernelrelease 2>/dev/null)
	make_status=$?
	if [ "$make_status" -ne 0 ]; then
		env_mismatch "make kernelrelease failed in ${KDIR} (status ${make_status})"
		kernel_release=unknown
	else
		kernel_release=$(printf '%s\n' "$kernelrelease_output" | awk 'NF { value = $0 } END { print value }')
	fi
	if [ "$make_status" -eq 0 ] && [ -z "$kernel_release" ]; then
		env_mismatch "could not read kernel release from ${KDIR}"
	elif [ "$make_status" -eq 0 ] && [ "$kernel_release" != 5.10.29 ]; then
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
	ptr_auth_unset_lines=$(awk '$0 == "# CONFIG_ARM64_PTR_AUTH is not set" { count++ } END { print count + 0 }' "$config_file")
	if [ "$ptr_auth_unset_lines" -lt 1 ]; then
		env_mismatch 'CONFIG_ARM64_PTR_AUTH must have an explicit unset line'
	elif [ "$ptr_auth_status" != unset ]; then
		env_mismatch "CONFIG_ARM64_PTR_AUTH is ${ptr_auth_status}, expected unset"
	fi
fi

if [ "$mismatch" -ne 0 ]; then
	exit 1
fi

hash_input=$(awk '
	/^CONFIG_[A-Za-z0-9_]+=/{ print }
	/^# CONFIG_[A-Za-z0-9_]+ is not set$/{ print }
' "$config_file" | LC_ALL=C sort)
if command -v sha256sum >/dev/null 2>&1; then
	hash_output=$(printf '%s\n' "$hash_input" | sha256sum)
	hash_status=$?
elif command -v shasum >/dev/null 2>&1; then
	hash_output=$(printf '%s\n' "$hash_input" | shasum -a 256)
	hash_status=$?
else
	env_mismatch 'neither sha256sum nor shasum is available'
	exit 1
fi

if [ "$hash_status" -ne 0 ]; then
	env_mismatch "config hash command failed (status ${hash_status})"
	exit 1
fi
config_hash=$(printf '%s\n' "$hash_output" | awk 'NF { print $1; exit }')
case "$config_hash" in
	''|*[!0123456789abcdefABCDEF]*)
		env_mismatch 'config hash is not a hexadecimal digest'
		exit 1
		;;
esac
if [ "${#config_hash}" -ne 64 ]; then
	env_mismatch "config hash has length ${#config_hash}, expected 64"
	exit 1
fi

printf 'KERNEL_RELEASE=%s\n' "$kernel_release"
printf 'ARCH=%s\n' "$arch"
printf 'KDIR=%s\n' "$KDIR"
printf 'CONFIG_HASH=%s\n' "$config_hash"
