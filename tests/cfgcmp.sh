#!/bin/bash
# Compares several build configurations of the decoder against each other.
#
# Two traps this avoids, both of which produced wrong answers before it
# existed:
#
#  - measuring one configuration fully before moving to the next. The position
#    of a run inside the measurement loop biases its result by several percent
#    on an otherwise idle machine: the first runs come out faster and it then
#    settles higher. A sweep done block by block therefore measures the loop
#    position rather than the variable, and it can invent an effect out of
#    nothing, or reverse a real one. Every configuration is run once per round
#    here, so any drift hits all of them equally.
#
#  - comparing single builds. Shifting the code by a few bytes moves the time
#    of an unchanged decoder by up to 12%, so each configuration is also built
#    with several .text paddings and the mean is reported.
#
# usage: tests/cfgcmp.sh [rounds] [pads] -- <cfg> [<cfg>...]
#        where each <cfg> is a set of -D flags, quoted.
#
# example, to find the best table widths on this machine:
#   tests/cfgcmp.sh 3 4 -- "-DUSLZ_FAST_BITS=0 -DUSLZ_FAST_DBITS=0" \
#                          "-DUSLZ_FAST_BITS=8 -DUSLZ_FAST_DBITS=6" \
#                          "-DUSLZ_FAST_BITS=9 -DUSLZ_FAST_DBITS=7"
ROUNDS=${1:-3}; PADS=${2:-4}; shift 2
[ "$1" = "--" ] && shift
[ -n "$1" ] || { sed -n '2,26p' "$0"; exit 1; }

S=$(TMPDIR=${TMPDIR:-/dev/shm} mktemp -d)
trap 'rm -rf "$S"' EXIT
CF="-O3 -fomit-frame-pointer -w -I$PWD/include -Isrc"

n=0
for cfg in "$@"; do
	echo "$cfg" > "$S/name.$n"
	for p in $(seq 0 $((PADS - 1))); do
		pad=$((p * 16 + 1))
		echo "__asm__(\".pushsection .text\\n.space $pad\\n.popsection\");" > "$S/pad.c"
		gcc $CF -c -o "$S/pad.o" "$S/pad.c" || exit 1
		gcc $CF $cfg -o "$S/z.$n.$p" "$S/pad.o" src/zdec.c src/slz_common.c \
		    src/uslz.c || exit 1
	done
	n=$((n + 1))
done

for f in ${CORPUS:-tests/silesia.tslz tests/silesia.tgz}; do
	cat "$f" > /dev/null
	: > "$S/res"
	for r in $(seq $ROUNDS); do
		for p in $(seq 0 $((PADS - 1))); do
			for i in $(seq 0 $((n - 1))); do
				t=$( { /usr/bin/time -f %e taskset -c 0 "$S/z.$i.$p" \
				       < "$f" > /dev/null; } 2>&1 | tail -1)
				echo "$i $p $t" >> "$S/res"
			done
		done
	done
	echo "--- ${f##*/}"
	awk -v dir="$S" '
	     { k = $1" "$2; if (!(k in m) || $3 + 0 < m[k]) m[k] = $3 + 0 }
	     END { for (k in m) { split(k, a, " "); s[a[1]] += m[k]; c[a[1]]++ }
	           for (i in s) {
	                 getline name < (dir "/name." i); close(dir "/name." i)
	                 printf "  %8.4f s   %s\n", s[i]/c[i], name
	           } }' "$S/res" | sort -n
done
