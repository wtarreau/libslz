#!/bin/bash
# usage: arlbuild.sh <out> [-D...]  builds zenc as Willy's ARL build (gcc-15, arrowlake-s)
O=$1; shift
cd ~/repo/libslz/20261107-lit-skip-1
F="-O3 -g -fomit-frame-pointer -march=arrowlake-s -mtune=arrowlake-s --param=l1-cache-size=32 --param=l1-cache-line-size=64 --param=l2-cache-size=30720 -Wall -Iinclude -fno-stack-protector -fno-stack-clash-protection -fcf-protection=none -fno-pie -no-pie -U_FORTIFY_SOURCE"
gcc-15 $F "$@" -DSLZ_VERSION='"x"' -o $O src/zenc.c src/slz_common.c src/slz.c
