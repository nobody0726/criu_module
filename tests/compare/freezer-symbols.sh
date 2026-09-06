#!/bin/sh
# A2 cgroup-v2 freezer feasibility preflight.
#
# This script is intentionally a guest-only gate. It does not load a module or
# change a task's cgroup; it only checks the kernel facilities required by the
# isolated compile probe and later freeze implementation.
set -eu

blocked() {
	echo "A2_FREEZER: BLOCKED: $*" >&2
	exit 77
}

[ "$(id -u)" = 0 ] || blocked "must run as root inside the guest"

# The project target is the aarch64 Linux 5.10 guest. Refuse to inspect a
# developer host by accident, even when that host has a cgroup hierarchy.
case "$(uname -m)" in
	aarch64|arm64) ;;
	*) blocked "expected an aarch64 guest, got $(uname -m)" ;;
esac
case "$(uname -r)" in
	5.10.*) ;;
	*) blocked "expected a 5.10.x guest kernel, got $(uname -r)" ;;
esac

DEBUGFS=/sys/kernel/debug
[ -d "$DEBUGFS" ] || blocked "$DEBUGFS is missing"
if ! grep -q ' /sys/kernel/debug ' /proc/mounts; then
	mount -t debugfs none "$DEBUGFS" 2>/dev/null ||
		blocked "cannot mount debugfs"
fi

CGROUP_ROOT=/sys/fs/cgroup
mkdir -p "$CGROUP_ROOT"
if ! grep -q " $CGROUP_ROOT cgroup2 " /proc/mounts; then
	mount -t cgroup2 none "$CGROUP_ROOT" 2>/dev/null ||
		blocked "cannot mount cgroup2 at $CGROUP_ROOT"
fi

[ -r "$CGROUP_ROOT/cgroup.controllers" ] ||
	blocked "cgroup2 controller list is unreadable"
grep -qw freezer "$CGROUP_ROOT/cgroup.controllers" ||
	blocked "cgroup2 freezer controller is unavailable"

config_file=
for candidate in \
	"/proc/config.gz" \
	"/boot/config-$(uname -r)" \
	"/lib/modules/$(uname -r)/config"; do
	if [ -r "$candidate" ]; then
		config_file=$candidate
		break
	fi
done

config_value() {
	key=$1
	if [ "${config_file:-}" = "/proc/config.gz" ]; then
		command -v zcat >/dev/null 2>&1 || return 2
		zcat "$config_file" 2>/dev/null | grep -E "^${key}=[ym]$"
	else
		grep -E "^${key}=[ym]$" "$config_file"
	fi
}

if [ -n "${config_file:-}" ]; then
	for key in CONFIG_CGROUP_FREEZER CONFIG_FREEZER; do
		if ! config_value "$key" >/dev/null 2>&1; then
			blocked "$key is disabled in $config_file"
		fi
	done
	echo "A2_FREEZER: CONFIG_PASS: $config_file"
else
	echo "A2_FREEZER: CONFIG_UNAVAILABLE: kernel config is not readable"
fi

echo "A2_FREEZER: CGROUP2_PASS: freezer controller available"
echo "A2_FREEZER: PASS"
exit 0
