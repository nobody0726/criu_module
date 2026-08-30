#!/bin/sh

set -u

RUN_DIR=${1:-}
[ -n "$RUN_DIR" ] || { printf 'usage: %s RUN_DIR\n' "$0" >&2; exit 2; }
[ -d "$RUN_DIR" ] || { printf 'S0: run directory missing: %s\n' "$RUN_DIR" >&2; exit 2; }

env_value()
{
	awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); exit }' \
		"$RUN_DIR/environment.txt"
}

KERNEL=$(env_value KERNEL_RELEASE)
ARCH=$(env_value ARCH)
CONFIG_HASH=$(env_value CONFIG_HASH)
[ -n "$KERNEL" ] || KERNEL=unknown
[ -n "$ARCH" ] || ARCH=unknown
[ -n "$CONFIG_HASH" ] || CONFIG_HASH=unknown

SUMMARY=$RUN_DIR/summary.tsv
printf 'id\tgroup\texpected_result\tobserved_result\tkernel\tarch\tconfig_hash\tevidence\treason\n' > "$SUMMARY" || exit 1

printf '%-6s %-5s %-22s %-16s %s\n' ID GROUP EXPECTED OBSERVED EVIDENCE
printf '%-6s %-5s %-22s %-16s %s\n' -- ----- ---------------------- ---------------- ---------------

selected=$RUN_DIR/selected.tsv
[ -r "$selected" ] || { printf 'S0: selected manifest is missing\n' >&2; exit 1; }

tab=$(printf '\t')
while IFS="$tab" read -r id group kind symbol expected gate fixture; do
	[ "$id" = id ] && continue
	[ -n "$id" ] || continue
	probe_dir=$RUN_DIR/$id
	result_file=$probe_dir/result.txt
	observed=MISSING
	reason='result file is missing'
	if [ -r "$result_file" ]; then
		observed=$(awk -F= '/^status=/ { print $2; exit }' "$result_file")
		reason=$(awk -F= '/^reason=/ { sub(/^reason=/, ""); print; exit }' "$result_file")
		[ -n "$observed" ] || observed=MALFORMED
		[ -n "$reason" ] || reason=none
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$id" "$group" "$expected" "$observed" "$KERNEL" "$ARCH" \
		"$CONFIG_HASH" "$probe_dir" "$reason" >> "$SUMMARY"
	printf '%-6s %-5s %-22s %-16s %s\n' "$id" "$group" "$expected" "$observed" "$probe_dir"
done < "$selected"

gate_status=0
awk -F '\t' '
NR == 1 { next }
{
	count++
	if ($4 == "MISSING" || $4 == "MALFORMED" ||
	    $4 == "CRASH" || $4 == "CLEANUP-FAIL" ||
	    $4 == "WRONG-VALUE" || $4 == "ENV-MISMATCH")
		bad = 1
	if (($3 == "OK" && $4 != "OK") ||
	    ($3 == "NO-SYMBOL" && $4 != "NO-SYMBOL") ||
	    ($3 == "UNSAFE" && $4 != "UNSAFE") ||
	    ($3 == "linkable-current-mm" && $4 != "OK"))
		bad = 1
	if ($1 == "1.2" && $4 == "OK") group1 = 1
	if ($1 == "1.3" && $4 == "OK") group1 = 1
	if ($1 == "2.4" && $4 == "OK") group2 = 1
	if ($1 == "3.2" && $4 == "OK") group3 = 1
	if ($1 == "3.3" && $4 == "OK") group3 = 1
	if ($2 == "1" && ($1 == "1.2" || $1 == "1.3")) seen1 = 1
	if ($2 == "2" && $1 == "2.4") seen2 = 1
	if ($2 == "3" && ($1 == "3.2" || $1 == "3.3")) seen3 = 1
}
END {
	if (count == 0 ||
	    (seen1 && !group1) || (seen2 && !group2) || (seen3 && !group3))
		bad = 1
	exit bad ? 1 : 0
}' "$SUMMARY" || gate_status=1

printf 'S0: summary written to %s\n' "$SUMMARY"
if [ "$gate_status" -ne 0 ]; then
	printf 'S0: acceptance gates failed\n' >&2
fi
exit "$gate_status"
