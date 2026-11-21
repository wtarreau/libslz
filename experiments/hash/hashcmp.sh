#!/bin/bash
# Builds zenc with slz_hash_bits() replaced by an expression of <a> (before the
# final >> (32 - bits), or with -DHASH_LOW its own & mask), with SLZ_HASH_BITS
# set, and prints the output sizes per level and file.
# usage: hashcmp.sh <libslz-src> <bits> "<levels>" "<files>" "<name>=<expr>[;low]"...
#   e.g. hashcmp.sh src 13 "1 2 3 4" "/dev/shm/silesia.tar" "mul=a*0x1af42f" "shift=(a<<19)+(a<<6)-a"
#        "fold=a^(a>>9)^(a>>18);low" uses the low bits instead of the top ones
I=$1; B=$2; LV=$3; F=$4; shift 4
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$I"/*.c "$I"/*.h $W/
python3 - $W/slz.c <<'PY'
import sys,re
p=sys.argv[1]; s=open(p).read()
i=s.index('uint32_t slz_hash_bits(uint32_t a, const int bits)\n{'); j=s.index('\n}\n',i)
s=s[:i]+'''uint32_t slz_hash_bits(uint32_t a, const int bits)
{
#ifdef HASH_LOW
	return (uint32_t)(SLZ_HASH_EXPR) & ((1u << bits) - 1);
#else
	return (uint32_t)(SLZ_HASH_EXPR) >> (32 - bits);
#endif'''+s[j:]
open(p,'w').write(s)
PY
for c in "$@"; do
	n=${c%%=*}; e=${c#*=}; low=""
	case $e in *";low") e=${e%;low}; low=-DHASH_LOW;; esac
	gcc -O3 -DSLZ_HASH_BITS=$B $low "-DSLZ_HASH_EXPR=$e" -I$W $W/zenc.c $W/slz.c $W/slz_common.c -o $W/z-$n || exit 1
	for f in $F; do
		printf "%-10s %2s bits %-14s" "$n" $B "$(basename $f)"
		for l in $LV; do printf " -%s:%10d" $l $($W/z-$n -$l -c $f | wc -c); done
		echo
	done
done
