#!/usr/bin/env bash
# Usage: bench.sh <cpu> <runs> <level> <file> <zenc>...
# Prints, for each zenc, the output size and the best input speed reported by
# zenc -t -v over <runs> runs (MB/s = 10^6 B/s), the builds being alternated
# from one run to the next.
cpu=$1 runs=$2 lev=$3 file=$4; shift 4
declare -A best size
for ((r = 0; r < runs; r++)); do
	for z in "$@"; do
		out=$(taskset -c $cpu $z -D -$lev -t -v -l 2 < "$file" 2>&1 >/dev/null)
		s=$(echo "$out" | grep -o 'totout=[0-9]*' | cut -d= -f2)
		v=$(echo "$out" | grep -o '[0-9.]*MB/s' | head -n1 | sed 's,MB/s,,')
		size[$z]=$s
		awk -v a="$v" -v b="${best[$z]:-0}" 'BEGIN{exit !(a > b)}' && best[$z]=$v
	done
done
for z in "$@"; do
	printf "%-28s -%s %12s %8s\n" "${z##*/}" "$lev" "${size[$z]}" "${best[$z]}"
done
