#!/usr/bin/env bash
# Builds the zenc variants of the study into <dir> (default ./b):
#   zenc-base         master's skip
#   zenc-noskip       -DSLZ_LITERAL_SKIP=0
#   zenc-s<S>f<F>     -DSLZ_STAT_SKIP=<S> -DSLZ_STAT_FN=<F>
# Run from the top of the tree. Extra CFLAGS may be passed in CFLAGS.
dir=${1:-b}
mkdir -p "$dir"
cc() { gcc -O3 -march=native -fomit-frame-pointer -Wall -Iinclude -Isrc $CFLAGS \
          -DSLZ_VERSION='"x"' -o "$dir/zenc-$1" src/zenc.c src/slz_common.c src/slz.c "${@:2}"; }
cc base
cc noskip -DSLZ_LITERAL_SKIP=0
for s in 1 3 5 7 2 10; do
	for f in 1 2 3 4; do
		cc s${s}f$f -DSLZ_STAT_SKIP=$s -DSLZ_STAT_FN=$f
	done
done
