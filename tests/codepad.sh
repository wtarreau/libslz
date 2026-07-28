#!/bin/bash
# Paired A/B benchmark for two versions of src/uslz.c, using zdec itself.
#
# The point of this script is that a single build tells you almost nothing
# here: shifting the code by a few bytes moves the decoding time of an
# *unchanged* decoder by up to 12% peak to peak, because the hot loop lands
# differently in the L1i, the branch predictors and the loop buffer. So each
# version is built with several .text paddings and the whole distribution is
# reported rather than a single sample. Only a difference that survives the
# padding sweep is a real difference.
#
# usage: tests/codepad.sh <a.c> <b.c> [pads] [runs]
#        CORPUS="f1 f2" tests/codepad.sh ...   to pick the corpora
A=$1; B=$2; PADS=${3:-8}; N=${4:-3}
[ -n "$B" ] || { sed -n '2,14p' "$0"; exit 1; }
S=$(TMPDIR=${TMPDIR:-/dev/shm} mktemp -d)
trap 'rm -rf "$S"' EXIT
CF="-O3 -fomit-frame-pointer -Wall -I$PWD/include -Isrc"

cp "$A" "$S/uslz_a.c" || exit 1
cp "$B" "$S/uslz_b.c" || exit 1

# one binary per (version, padding); the padding object comes first in the
# link order so that everything after it shifts by that many bytes.
for p in $(seq 0 $((PADS - 1))); do
	pad=$((p * 16 + 1))
	echo "__asm__(\".pushsection .text\\n.space $pad\\n.popsection\");" > "$S/pad.c"
	gcc $CF -c -o "$S/pad.o" "$S/pad.c" || exit 1
	for v in a b; do
		gcc $CF -DSLZ_VERSION='"test"' -c -o "$S/uslz_$v.o" "$S/uslz_$v.c" || exit 1
		gcc $CF -o "$S/zdec_${v}_$p" "$S/pad.o" src/zdec.c src/slz_common.c \
		    "$S/uslz_$v.o" || exit 1
	done
done

for f in ${CORPUS:-tests/silesia.tslz tests/silesia.tgz}; do
	cat "$f" > /dev/null
	echo "--- ${f##*/}"
	printf '  %-6s %8s %8s\n' pad A B
	for p in $(seq 0 $((PADS - 1))); do
		ba=999; bb=999
		for i in $(seq $N); do
			for v in a b; do
				t=$( { /usr/bin/time -f %e taskset -c 0 "$S/zdec_${v}_$p" \
				       < "$f" > /dev/null; } 2>&1 )
				if [ $v = a ]; then
					ba=$(awk -v x="$t" -v y="$ba" 'BEGIN{print (x+0<y+0)?x+0:y+0}')
				else
					bb=$(awk -v x="$t" -v y="$bb" 'BEGIN{print (x+0<y+0)?x+0:y+0}')
				fi
			done
		done
		printf '  %-6s %8s %8s\n' $((p * 16 + 1)) "$ba" "$bb"
		echo "$ba $bb" >> "$S/res"
	done
	awk '{sa += $1; sb += $2; n++
	      if (n == 1 || $1 < mina) mina = $1
	      if (n == 1 || $2 < minb) minb = $2}
	     END{printf "  mean   %8.4f %8.4f   B/A %+6.2f%%\n", sa/n, sb/n, (sb/sa-1)*100
	         printf "  best   %8.4f %8.4f   B/A %+6.2f%%\n", mina, minb, (minb/mina-1)*100}' "$S/res"
	rm -f "$S/res"
done
printf 'text size:   A %s   B %s\n' "$(size "$S/uslz_a.o" | awk 'NR==2{print $1}')" \
                                    "$(size "$S/uslz_b.o" | awk 'NR==2{print $1}')"
