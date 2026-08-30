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

function_targets='find_get_task_by_vpid|find_vpid|pid_task|get_pid_task|put_task_struct|get_task_mm|mmput|mmget_not_zero|mmap_read_lock|d_path|follow_page|get_user_pages_remote|access_process_vm|mm_alloc|vm_area_alloc|insert_vm_struct|do_mmap|vm_mmap|do_munmap|vm_munmap|alloc_pid|kernel_execve|vm_insert_page'
field_targets='vm_next|vm_file'
all_targets="${function_targets}|${field_targets}"
function_matches=$(rg -n -H --glob '*.[ch]' -e "(^|[^[:alnum:]_])(${function_targets})[[:space:]]*\\(" include kernel mm fs 2>/dev/null || true)
field_matches=$(rg -n -H --glob '*.[ch]' -e "(^|[^[:alnum:]_])(${field_targets})[[:space:]]*[,;]" include kernel mm fs 2>/dev/null || true)
export_matches=$(rg -n -H --glob '*.[ch]' -e "^[[:space:]]*EXPORT_SYMBOL(_GPL)?\\((${all_targets})\\)[[:space:]]*;" . 2>/dev/null || true)

function_declaration()
{
	name=$1
	declaration=$(printf '%s\n' "$function_matches" | awk -F: -v name="$name" '
		BEGIN { function_re = "(^|[^[:alnum:]_])" name "[[:space:]]*[(]" }
		{
			path = $1
			line = $0
			sub(/^[^:]*:[0-9]+:/, "", line)
			if (line !~ function_re)
				next
			if (substr(path, 1, 8) == "include/" &&
			    substr(line, 1, 1) != "*" &&
			    substr(line, 1, 2) != "//") {
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
				if (line ~ function_re) {
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
			if (path == "include/linux/mm_types.h" && line ~ field_re) {
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
			printf 'generic'
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
			printf 'declaration/export inspected in the Linux 5.10.29 source tree'
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
