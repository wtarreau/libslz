#!/bin/bash
# usage: epad.sh <a.c> <b.c> <pads> <runs> <level> <file>...   (run from the repo)
A=$1 B=$2 PADS=$3 N=$4 L=$5; shift 5
W=$(mktemp -d /dev/shm/epad.XXXX); trap 'rm -rf $W' EXIT
CF="-O3 -march=native -fomit-frame-pointer -Wa,-mbranches-within-32B-boundaries -Wa,-malign-branch=jcc+fused+jmp+call -Iinclude -Isrc -DSLZ_VERSION=\"x\""
for p in $(seq 0 $((PADS-1))); do
	echo "__asm__(\".pushsection .text\\n.space $((p*16+1))\\n.popsection\");" > $W/pad.c
	gcc $CF -c -o $W/pad.o $W/pad.c
	gcc $CF -o $W/a$p $W/pad.o src/zenc.c src/slz_common.c $A
	gcc $CF -o $W/b$p $W/pad.o src/zenc.c src/slz_common.c $B
done
for f in "$@"; do
	for p in $(seq 0 $((PADS-1))); do
		ba=0; bb=0
		for i in $(seq $N); do for v in a b; do
			x=$(taskset -c 2 $W/$v$p -D -$L -t -v -l 2 < $f 2>&1 >/dev/null | grep -o '[0-9.]*MB/s' | head -1 | sed 's,MB/s,,')
			if [ $v = a ]; then ba=$(awk -v x=$x -v y=$ba 'BEGIN{print ((x+0>y+0)?x:y)}'); else bb=$(awk -v x=$x -v y=$bb 'BEGIN{print ((x+0>y+0)?x:y)}'); fi
		done; done
		echo "$ba $bb"
	done | awk -v f=${f##*/} -v l=$L '{sa+=$1; sb+=$2; n++; s=s sprintf(" %.0f/%.0f",$1,$2)} END{printf "%-12s -%s A %.1f B %.1f  B/A %+.2f%%  [%s ]\n", f, l, sa/n, sb/n, 100*(sb/sa-1), s}'
done
