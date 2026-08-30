#!/bin/sh

set -u

LINUX_SRC=${LINUX_SRC:-/Users/yhome/workspace/source_code/linux-5.10.29}

if [ ! -d "$LINUX_SRC" ]; then
	printf 'SOURCE-MISMATCH: source tree does not exist: %s\n' "$LINUX_SRC" >&2
	exit 1
fi
if ! command -v rg >/dev/null 2>&1; then
	printf 'SOURCE-MISMATCH: rg is required for the read-only source audit\n' >&2
	exit 1
fi

cd "$LINUX_SRC" || exit 1

scan_dirs='include kernel mm fs'
for scan_dir in $scan_dirs
do
	if [ ! -d "$scan_dir" ]; then
		printf 'SOURCE-MISMATCH: required scan directory does not exist: %s\n' "$LINUX_SRC/$scan_dir" >&2
		exit 1
	fi
done

function_targets='find_get_task_by_vpid|find_vpid|pid_task|get_pid_task|put_task_struct|get_task_mm|mmput|mmget_not_zero|mmap_read_lock|d_path|follow_page|get_user_pages_remote|access_process_vm|mm_alloc|vm_area_alloc|insert_vm_struct|do_mmap|vm_mmap|do_munmap|vm_munmap|alloc_pid|kernel_execve|vm_insert_page'
field_targets='vm_next|vm_file'
all_targets="${function_targets}|${field_targets}"
scan_rg()
{
	rg -n -H --glob '*.[ch]' -e "$1" $scan_dirs
	rg_status=$?
	case "$rg_status" in
		0|1)
			return 0
			;;
		*)
			return "$rg_status"
			;;
	esac
}

function_matches=$(scan_rg "(^|[^[:alnum:]_])(${function_targets})[[:space:]]*\\(")
scan_status=$?
if [ "$scan_status" -ne 0 ]; then
	printf 'SOURCE-MISMATCH: function scan failed (status %s)\n' "$scan_status" >&2
	exit 1
fi
function_matches=$(printf '%s\n' "$function_matches" | LC_ALL=C sort -u)

field_matches=$(scan_rg "(^|[^[:alnum:]_])(${field_targets})[[:space:]]*[,;]")
scan_status=$?
if [ "$scan_status" -ne 0 ]; then
	printf 'SOURCE-MISMATCH: field scan failed (status %s)\n' "$scan_status" >&2
	exit 1
fi
field_matches=$(printf '%s\n' "$field_matches" | LC_ALL=C sort -u)

export_matches=$(scan_rg "^[[:space:]]*EXPORT_SYMBOL(_GPL)?\\((${all_targets})\\)[[:space:]]*;")
scan_status=$?
if [ "$scan_status" -ne 0 ]; then
	printf 'SOURCE-MISMATCH: export scan failed (status %s)\n' "$scan_status" >&2
	exit 1
fi
export_matches=$(printf '%s\n' "$export_matches" | LC_ALL=C sort -u)

function_declaration()
{
	name=$1
	declaration=$(printf '%s\n' "$function_matches" | awk -F: -v name="$name" '
		BEGIN { function_re = "(^|[^[:alnum:]_])" name "[[:space:]]*[(]" }
		{
			path = $1
			line = $0
			sub(/^[^:]*:[0-9]+:/, "", line)
			sub(/^[[:space:]]+/, "", line)
			if (line !~ function_re)
				next
			if (substr(path, 1, 8) == "include/" &&
			    substr(line, 1, 1) != "/" && substr(line, 1, 1) != "*") {
				prefix = substr(line, 1, index(line, name) - 1)
				if (prefix ~ /(^|[[:space:]])(return|if|while|for|switch)[[:space:]]*$/ ||
				    prefix ~ /[.=]>[[:space:]]*$/)
					next
				print path ":" $2
				exit
			}
		}
	')
	if [ -z "$declaration" ]; then
		declaration=$(printf '%s\n' "$function_matches" | awk -F: -v name="$name" '
			BEGIN { function_re = "(^|[^[:alnum:]_])" name "[[:space:]]*[(]" }
			{
				line = $0
				sub(/^[^:]*:[0-9]+:/, "", line)
				sub(/^[[:space:]]+/, "", line)
				if (line ~ function_re && substr(line, 1, 1) != "/" &&
				    substr(line, 1, 1) != "*") {
					prefix = substr(line, 1, index(line, name) - 1)
					if (prefix ~ /(^|[[:space:]])(return|if|while|for|switch)[[:space:]]*$/ ||
					    prefix ~ /[.=]>[[:space:]]*$/)
						next
					print $1 ":" $2
					exit
				}
			}
		')
	fi
	if [ -n "$declaration" ]; then
		printf '%s' "$declaration"
	else
		printf 'missing'
	fi
}

field_declaration()
{
	name=$1
	declaration=$(printf '%s\n' "$field_matches" | awk -F: -v name="$name" '
		BEGIN { field_re = "(^|[^[:alnum:]_])" name "[[:space:]]*[,;]" }
		{
			path = $1
			line = $0
			sub(/^[^:]*:[0-9]+:/, "", line)
			sub(/^[[:space:]]+/, "", line)
			if (path == "include/linux/mm_types.h" && line ~ field_re &&
			    substr(line, 1, 1) != "/" && substr(line, 1, 1) != "*") {
				print path ":" $2
				exit
			}
		}
	')
	if [ -n "$declaration" ]; then
		printf '%s' "$declaration"
	else
		printf 'missing'
	fi
}

export_locations()
{
	name=$1
	printf '%s\n' "$export_matches" | awk -F: -v name="$name" '
			{
				sub(/^\.\//, "", $1)
				line = $0
				sub(/^[^:]*:[0-9]+:/, "", line)
				if (index(line, "EXPORT_SYMBOL(" name ")") > 0 ||
				    index(line, "EXPORT_SYMBOL_GPL(" name ")") > 0) {
					print $1 ":" $2
				}
			}
		' |
		awk '
			BEGIN { separator = "" }
			{
				printf "%s%s", separator, $0
				separator = ","
			}
			END { if (separator == "") printf "none" }
		'
}

export_flavors()
{
	name=$1
	printf '%s\n' "$export_matches" | awk -F: -v name="$name" '
			BEGIN { separator = "" }
			{
				line = $0
				sub(/^[^:]*:[0-9]+:/, "", line)
				if (index(line, "EXPORT_SYMBOL_GPL(" name ")") > 0)
					flavor = "EXPORT_SYMBOL_GPL"
				else if (index(line, "EXPORT_SYMBOL(" name ")") > 0)
					flavor = "EXPORT_SYMBOL"
				else
					next
				printf "%s%s", separator, flavor
				separator = ","
			}
			END { if (separator == "") printf "none" }
		'
}

scope_for()
{
	case "$1" in
		do_mmap|do_munmap)
			printf 'MMU+NOMMU'
			;;
		vm_mmap|vm_munmap)
			printf 'MMU+NOMMU'
			;;
		vm_insert_page)
			printf 'MMU+NOMMU'
			;;
		vm_next|vm_file)
			printf 'MMU+NOMMU'
			;;
		*)
			printf 'MMU+NOMMU'
			;;
	esac
}

notes_for()
{
	case "$1" in
		do_mmap)
			printf 'MMU implementation: mm/mmap.c; NOMMU implementation: mm/nommu.c'
			;;
		do_munmap)
			printf 'MMU implementation: mm/mmap.c; NOMMU implementation: mm/nommu.c'
			;;
		vm_mmap)
			printf 'wrapper uses current->mm; it cannot select a target task mm; MMU/NOMMU semantics are implementation-specific'
			;;
		vm_munmap)
			printf 'wrapper uses current->mm; it cannot select a target task mm; MMU/NOMMU semantics are implementation-specific'
			;;
		vm_insert_page)
			printf 'MMU: mm/memory.c; NOMMU: mm/nommu.c; may set VM_MIXEDMAP on the VMA'
			;;
		vm_next|vm_file)
			printf 'vm_area_struct field; MMU and NOMMU implementations use configuration-dependent VMA layouts'
			;;
		*)
			printf 'MMU: common kernel API declaration/export inspected; NOMMU: common kernel API declaration/export inspected'
			;;
	esac
}

for target in \
	find_get_task_by_vpid find_vpid pid_task get_pid_task put_task_struct \
	get_task_mm mmput mmget_not_zero mmap_read_lock vm_next vm_file d_path \
	follow_page get_user_pages_remote access_process_vm mm_alloc vm_area_alloc \
	insert_vm_struct do_mmap vm_mmap do_munmap vm_munmap alloc_pid kernel_execve \
	vm_insert_page
do
	case "$target" in
		vm_next|vm_file)
			declaration=$(field_declaration "$target")
			;;
		*)
			declaration=$(function_declaration "$target")
			;;
	esac
	locations=$(export_locations "$target")
	flavors=$(export_flavors "$target")
	scope=$(scope_for "$target")
	notes=$(notes_for "$target")
	printf 'api=%s\tdeclaration=%s\texport=%s\texport_flavor=%s\tscope=%s\tnotes=%s\n' \
		"$target" "$declaration" "$locations" "$flavors" "$scope" "$notes"
done
