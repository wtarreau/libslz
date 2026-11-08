#!/bin/bash
# usage: cyc.sh "<bins>" "<zenc args>" file rounds -> min cycles, instructions, MB/s best, size
B=$1; A=$2; F=$3; R=${4:-5}
declare -A c i m o
for r in $(seq $R); do for b in $B; do
  out=$(perf stat -x, -e cycles,instructions taskset -c 4 ./$b -t -v $A < $F 2>&1 >/dev/null)
  cy=$(echo "$out" | awk -F, '/cycles/{print $1}'); in=$(echo "$out" | awk -F, '/instructions/{print $1}')
  mb=$(echo "$out" | sed -n 's/.* \([0-9.]*\)MB\/s saving.*/\1/p'); sz=$(echo "$out" | sed -n 's/.*totout=\([0-9]*\).*/\1/p')
  [ -z "${c[$b]}" ] || [ $cy -lt ${c[$b]} ] && c[$b]=$cy; i[$b]=$in; o[$b]=$sz
  m[$b]=$(awk -v a="${m[$b]:-0}" -v b=$mb 'BEGIN{print (b>a)?b:a}')
done; done
f=${B%% *}
for b in $B; do awk -v n=$b -v c=${c[$b]} -v c0=${c[$f]} -v i=${i[$b]} -v i0=${i[$f]} -v m=${m[$b]} -v o=${o[$b]} 'BEGIN{printf "  %-8s cyc %+6.2f%%  ins %+6.2f%%  %7.1f MB/s  out %d\n", n, (c/c0-1)*100, (i/i0-1)*100, m, o}'; done
