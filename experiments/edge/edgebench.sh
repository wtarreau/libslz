#!/bin/bash
# usage: edgebench.sh <dir with zenc-base zenc-edge zenc-keep> [rounds] [cpu]
# best-of-N zenc input MB/s and output size, builds alternated within each round
D=$1; R=${2:-5}; CPU=${3:-4}
V="base edge keep"
sp(){ taskset -c $CPU $D/zenc-$1 $2 -t -v -l 3 $3 2>&1 | sed -n 's/.*totout=\([0-9]*\).* \([0-9.]*\)MB\/s saving.*/\1 \2/p'; }
for f in /dev/shm/silesia.tar /dev/shm/lin200.tar; do for k in 32 4096; do for l in -1 -2 -3 -4 -T; do
	declare -A b=() o=()
	for r in $(seq $R); do for v in $V; do read sz s < <(sp $v "$l -k $k" $f); o[$v]=$((sz/3)); b[$v]=$(awk -v a="${b[$v]:-0}" -v b=$s 'BEGIN{print (b>a)?b:a}'); done; done
	printf '%-12s k=%-5s %s' ${f##*/} $k $l
	for v in $V; do printf '  %s %7s' $v ${b[$v]}; done
	awk -v a=${b[base]} -v e=${b[edge]} -v k=${b[keep]} -v oa=${o[base]} -v oe=${o[edge]} 'BEGIN{printf "  edge %+.2f%% keep %+.2f%%  out %+.4f%%\n", (e/a-1)*100, (k/a-1)*100, (oe/oa-1)*100}'
	unset b o
done; done; done
