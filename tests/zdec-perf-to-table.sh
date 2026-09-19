#!/bin/bash
# Turns a directory of tests/zdec-perf.sh outputs into the before/after MB/s
# table used in commit messages, plus the per-run deltas.
#
# usage: zdec-perf-table.sh <dir> [compiler]
#
# Files are named <machine>.out or <machine>-<compiler>.out. For machines
# having several, the table picks <machine>-<compiler>.out (default gcc15).
# "before" is the [base] line and "after" the last commit of the series.
D=${1:?usage: $0 <dir> [compiler]}; CC=${2:-gcc15}
MACHINES="arl spr sklx 7800 7v13 5105 atom n1 a720"

# prints "slz_before slz_after gzip_before gzip_after text_before text_after"
extract() {
	awk '/^=== every commit/ {s = 1; next}
	     s && /^=== / {exit}
	     s && /^\[base\]/ {bs = $(NF-3); bg = $(NF-2); bt = $(NF-1)}
	     s && NF >= 5 && !/^commit / {as = $(NF-3); ag = $(NF-2); at = $(NF-1)}
	     END {print bs, as, bg, ag, bt, at}' "$1"
}

pick() {
	if [ -e "$D/$1-$CC.out" ]; then echo "$D/$1-$CC.out"
	elif [ -e "$D/$1.out" ]; then echo "$D/$1.out"
	fi
}

r=(); for m in $MACHINES; do
	f=$(pick $m)
	if [ -n "$f" ]; then r+=("$(extract "$f")"); else r+=("- - - - - -"); fi
done

row() { # <label> <field>
	local i=$2
	printf '    %-11s' "$1"
	for v in "${r[@]}"; do
		set -- $v; x=${!i}
		[ "$x" = - ] && printf ' %4s' - || printf ' %4d' "$(awk -v x=$x 'BEGIN{print int(x + 0.5)}')"
	done; echo
}

printf '%15s' ''; for m in $MACHINES; do printf ' %4s' $m; done; echo
row "slz  before" 1; row "     after" 2
row "gzip before" 3; row "     after" 4

echo; echo "per run:"
for f in "$D"/*.out; do
	set -- $(extract "$f")
	[ -n "$2" ] || { printf '  %-12s no tip commit\n' "$(basename $f .out)"; continue; }
	awk -v n="$(basename $f .out)" -v bs=$1 -v as=$2 -v bg=$3 -v ag=$4 -v bt=$5 -v at=$6 \
	    'BEGIN{printf "  %-12s slz %+6.2f%%  gzip %+6.2f%%  text %+d\n", n, (as/bs-1)*100, (ag/bg-1)*100, at-bt}'
done
