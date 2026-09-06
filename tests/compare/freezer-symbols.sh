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
mkdir -p "$CGROUP_ROOT" || blocked "cannot create $CGROUP_ROOT"
if ! grep -q " $CGROUP_ROOT cgroup2 " /proc/mounts; then
	mount -t cgroup2 none "$CGROUP_ROOT" 2>/dev/null ||
		blocked "cannot mount cgroup2 at $CGROUP_ROOT"
fi

PROBE_CGROUP="$CGROUP_ROOT/criu-a2-probe.$$"
mkdir "$PROBE_CGROUP" || blocked "cannot create probe cgroup"
cleanup_probe() {
	echo 0 >"$PROBE_CGROUP/cgroup.freeze" 2>/dev/null || true
	rmdir "$PROBE_CGROUP" 2>/dev/null || true
}
trap cleanup_probe EXIT HUP INT TERM
[ -w "$PROBE_CGROUP/cgroup.freeze" ] || blocked "probe cgroup.freeze is not writable"
[ -r "$PROBE_CGROUP/cgroup.events" ] || blocked "probe cgroup.events is unreadable"
grep -q 'frozen 0' "$PROBE_CGROUP/cgroup.events" ||
	blocked "probe cgroup starts frozen"
echo 1 >"$PROBE_CGROUP/cgroup.freeze" ||
	blocked "cannot freeze probe cgroup"
grep -q 'frozen 1' "$PROBE_CGROUP/cgroup.events" ||
	blocked "probe cgroup did not report frozen"
echo 0 >"$PROBE_CGROUP/cgroup.freeze" ||
	blocked "cannot thaw probe cgroup"
grep -q 'frozen 0' "$PROBE_CGROUP/cgroup.events" ||
	blocked "probe cgroup did not report thawed"

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
	for key in CONFIG_CGROUPS; do
		if ! config_value "$key" >/dev/null 2>&1; then
			blocked "$key is disabled in $config_file"
		fi
	done
	echo "A2_FREEZER: CONFIG_PASS: $config_file"
else
	echo "A2_FREEZER: CONFIG_UNAVAILABLE: kernel config is not readable"
fi

echo "A2_FREEZER: CGROUP2_PASS: child freezer files functional"
echo "A2_FREEZER: PASS"
exit 0
