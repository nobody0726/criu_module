#!/bin/sh

set -u

RUN_DIR=${1:-}
[ -n "$RUN_DIR" ] || { printf 'usage: %s RUN_DIR\n' "$0" >&2; exit 2; }
[ -d "$RUN_DIR" ] || { printf 'S0: run directory missing: %s\n' "$RUN_DIR" >&2; exit 2; }

failures=0
selected=$RUN_DIR/selected.tsv
summary=$RUN_DIR/summary.tsv
[ -r "$selected" ] || { printf 'S0: selected manifest is missing\n' >&2; exit 1; }
[ -r "$summary" ] || { printf 'S0: summary is missing\n' >&2; exit 1; }

if [ ! -r "$RUN_DIR/environment.txt" ] || [ ! -r "$RUN_DIR/source-audit.txt" ]; then
	printf 'S0: environment or source-audit evidence is missing\n' >&2
	exit 1
fi

expected_for()
{
	case "$1" in
		OK|NO-SYMBOL|UNSAFE) printf '%s' "$1" ;;
	linkable-current-mm) printf 'OK' ;;
	*) printf 'INVALID' ;;
	esac
}

new_dmesg()
{
	before_lines=$(wc -l < "$1")
	tail -n +$((before_lines + 1)) "$2" | grep -E \
		'BUG:|WARNING:|Oops|panic|KASAN|use-after-free|possible circular locking|sleeping function called|suspicious RCU usage' || true
}

while IFS='	' read -r id group kind symbol expected gate fixture; do
	[ "$id" = id ] && continue
	[ -n "$id" ] || continue
	probe_dir=$RUN_DIR/$id
	result_file=$probe_dir/result.txt
	status=
	if [ -r "$result_file" ]; then
		status=$(awk -F= '/^status=/ { print $2; exit }' "$result_file")
	fi

	if [ ! -r "$result_file" ] || [ -z "$status" ]; then
		printf 'S0 CHECK: %s missing or malformed result\n' "$id" >&2
		failures=1
		continue
	fi

	case "$kind" in
		compile)
			[ -r "$probe_dir/build.log" ] || {
				printf 'S0 CHECK: %s build.log missing\n' "$id" >&2
				failures=1
			}
			[ -r "$probe_dir/modpost.log" ] || {
				printf 'S0 CHECK: %s modpost.log missing\n' "$id" >&2
				failures=1
			}
			if [ "$expected" = NO-SYMBOL ]; then
				grep -Eq 'modpost: "[^" ]+" .* undefined!' "$probe_dir/modpost.log" || {
					printf 'S0 CHECK: %s lacks unresolved-symbol evidence\n' "$id" >&2
					failures=1
				}
			fi
			;;
		runtime)
			for evidence in qemu.log status report vmas dmesg-before dmesg-after; do
				[ -r "$probe_dir/$evidence" ] || {
					printf 'S0 CHECK: %s/%s missing\n' "$id" "$evidence" >&2
					failures=1
				}
			done
			grep -q "^S0_RESULT: $id:" "$probe_dir/qemu.log" || {
				printf 'S0 CHECK: %s result marker missing\n' "$id" >&2
				failures=1
			}
			grep -q 'framework unloaded for probe' "$probe_dir/qemu.log" || {
				printf 'S0 CHECK: %s module unload evidence missing\n' "$id" >&2
				failures=1
			}
			if [ -r "$probe_dir/dmesg-before" ] && [ -r "$probe_dir/dmesg-after" ]; then
				dirty=$(new_dmesg "$probe_dir/dmesg-before" "$probe_dir/dmesg-after")
				if [ "$id" = 1.4 ]; then
					dirty=$(printf '%s\n' "$dirty" | grep -v 'suspicious RCU usage' || true)
				fi
				if [ -n "$dirty" ]; then
					printf 'S0 CHECK: %s introduced kernel diagnostics:\n%s\n' "$id" "$dirty" >&2
					failures=1
				fi
			fi
			;;
		*)
			printf 'S0 CHECK: %s has unsupported kind %s\n' "$id" "$kind" >&2
			failures=1
			;;
	esac

	accepted=$(expected_for "$expected")
	if [ "$accepted" = INVALID ] || [ "$status" != "$accepted" ]; then
		printf 'S0 CHECK: %s expected %s but observed %s\n' "$id" "$expected" "$status" >&2
		failures=1
	fi

	if [ "$id" = 1.4 ]; then
		grep -q '^reason=missing RCU protection$' "$probe_dir/report" 2>/dev/null || {
			printf 'S0 CHECK: 1.4 unsafe reason missing\n' >&2
			failures=1
		}
	fi
	if [ "$id" = 4.8 ]; then
		grep -q '^vm_mixedmap_added=1$' "$probe_dir/report" 2>/dev/null || {
			printf 'S0 CHECK: 4.8 VM_MIXEDMAP evidence missing\n' >&2
			failures=1
		}
		grep -q '^smaps_delta=changed$' "$probe_dir/report" 2>/dev/null || {
			printf 'S0 CHECK: 4.8 smaps delta evidence missing\n' >&2
			failures=1
		}
	fi
done < "$selected"

manifest_count=$(awk 'NR > 1 && NF { count++ } END { print count + 0 }' "$selected")
summary_count=$(awk 'NR > 1 && NF { count++ } END { print count + 0 }' "$summary")
if [ "$manifest_count" -ne "$summary_count" ]; then
	printf 'S0 CHECK: selected rows=%s summary rows=%s\n' "$manifest_count" "$summary_count" >&2
	failures=1
fi

if [ "$failures" -ne 0 ]; then
	printf 'S0 CHECK: FAIL\n' >&2
	exit 1
fi
printf 'S0 CHECK: PASS\n'
exit 0
