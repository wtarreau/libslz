#!/bin/bash
# Build and time the uslz decoder at every commit of the current series, and
# at the tip under the build options the commit messages and the slz.h comment
# talk about, so that the same table can be filled in on another machine.
#
# It deliberately does not pin itself to a CPU: run it under taskset yourself.
#
#   taskset -c 4 tests/zdec-perf.sh
#
# The results quoted in the commit messages of this series, and in the comment
# above USLZ_FAST_BITS in slz.h, come from this script run on:
#
#   arl    Intel Core Ultra 7 265K          (Arrow Lake)
#   spr    Intel Xeon w3-2435               (Sapphire Rapids)
#   skx    Intel Xeon W-2145                (Skylake-X, jump padding on)
#   7800   AMD Ryzen 7 7800X3D              (Zen 4)
#   7v13   AMD EPYC 7V13                    (Zen 3)
#   5105   Intel Celeron N5105              (Jasper Lake)
#   atom   Intel Atom x5-Z8350              (Cherry Trail)
#   n1     Ampere Altra Q80-26              (Neoverse-N1)
#   a720   CIX P1 CD8180                    (Cortex-A720)
#
# Corpora, both expected to be silesia.tar compressed two ways (any file will
# do, the numbers are only ever compared against each other):
#   SLZ=/dev/shm/silesia.tslz   produced by zenc (fixed huffman)
#   GZ=/dev/shm/silesia.tgz     produced by gzip (dynamic huffman)
#
# Other knobs:
#   CC=gcc-14      compiler to use                  (default: cc)
#   BASE=master    where to start; it is measured too, as the first row
#   TIP=HEAD       last commit, included            (default: HEAD)
#   REPEAT=2       runs per configuration, best kept (default: 2)
#   MODE=full      "commits", "options" or "full"   (default: full)
#   OPTS=...       which option groups to measure, "widths" and/or "prefetch"
#                  (default: both)
#   CFLAGS=...     flags *appended* to the ones below, for anything a given
#                  platform needs. On a Skylake-X that means
#                  CFLAGS="-Wa,-mbranches-within-32B-boundaries
#                  -Wa,-malign-branch=jcc+fused+jmp+call", without which
#                  that core drops the decoder's jumps out of its uop cache
#                  and every number comes out some 8% low; the first option
#                  does not pad calls, which costs gcc 14 another 2% on the
#                  call to memcpy() in the match copy. With gcc 14 or
#                  older on a target that has avx512f, -mno-avx512f belongs
#                  here too: it is worth about 1.5% on the fixed stream there,
#                  and much more to the encoder, which this script does not
#                  measure. Never pass it to gcc 15, which loses 4 to 9% from
#                  it; see the note above uslz_decode_block().
#   WIDTHS=...     '|' separated "lit dist" pairs   (default: the seven below)
#   PREFETCH=...   USLZ_PREFETCH values to try      (default: 0 1), skipped
#                  when the sources being built do not have that setting
#
# To refresh only what a change to one patch invalidates, start from its parent
# and ask for the option groups whose numbers are quoted somewhere:
#
#   BASE=<that patch>^ OPTS=widths,prefetch taskset -c 4 tests/zdec-perf.sh
#
# A run takes a few minutes. The best of the repeats is kept rather than the
# mean: an occasional run comes out 4% low here for no visible reason, while
# the fast ones are all within half a percent of each other.
#
# Everything is built from "git archive" of each commit into a temporary
# directory, so the working tree is never touched and the script can run from
# a dirty checkout.

set -u
CC=${CC:-cc}
SLZ=${SLZ:-/dev/shm/silesia.tslz}
GZ=${GZ:-/dev/shm/silesia.tgz}
BASE=${BASE:-master}
TIP=${TIP:-HEAD}
REPEAT=${REPEAT:-2}
MODE=${MODE:-full}
OPTS=${OPTS:-widths,prefetch}
WIDTHS=${WIDTHS:-"10 7|0 0|0 7|9 0|8 6|9 7|11 7"}
PREFETCH=${PREFETCH:-"0 1"}

want() { case ",$OPTS," in *",$1,"*) return 0;; esac; return 1; }

command -v "$CC" >/dev/null || { echo "no such compiler: $CC" >&2; exit 1; }
for f in "$SLZ" "$GZ"; do
	[ -r "$f" ] || { echo "missing corpus: $f" >&2; exit 1; }
done
top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not a git repo" >&2; exit 1; }
cd "$top" || exit 1

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# Only keep the flags this compiler and this target actually accept, so that
# the same script runs on x86 and on arm.
probe() {
	echo 'int main(void){return 0;}' > "$tmp/probe.c"
	$CC $1 -o "$tmp/probe" "$tmp/probe.c" >/dev/null 2>&1 && echo "$1"
}
ARCH=$(probe -march=native)
[ -n "$ARCH" ] || ARCH=$(probe -mcpu=native)
# avx512f is deliberately left enabled: uslz_decode_block() caps its own
# vector width where that matters, and from gcc-15 on, turning avx512f off
# costs the decoder 4 to 9%. See the CFLAGS knob above for the compilers that
# still want -mno-avx512f.
CFLAGS="-O3 -fomit-frame-pointer -Wall $ARCH ${CFLAGS:-}"

cpu=$(sed -n 's/^model name[ \t]*: //p;s/^Model[ \t]*: //p' /proc/cpuinfo 2>/dev/null | head -1)
[ -n "$cpu" ] || cpu=$(uname -m)
echo "# machine : $(uname -m) | $cpu"
echo "# compiler: $($CC --version | head -1)"
echo "# flags   : $CFLAGS"
echo "# corpora : $SLZ ($(stat -c %s "$SLZ") B), $GZ ($(stat -c %s "$GZ") B)"
echo "# repeats : best of $REPEAT run(s) of 'zdec -t -v -l 3' per configuration"
echo

# build <srcdir> <outfile> <extra cflags...>
build() {
	local d=$1 out=$2; shift 2
	$CC $CFLAGS "$@" -I"$d/src" -DSLZ_VERSION='"zdec-perf"' \
	    -o "$out" "$d/src/zdec.c" "$d/src/slz_common.c" "$d/src/uslz.c" 2>"$tmp/cc.log"
}

# one number out of zdec, or "-" if it would not run
speed() {
	local bin=$1 f=$2 best= out=
	local i=0
	while [ $i -lt "$REPEAT" ]; do
		out=$("$bin" -t -v -l 3 "$f" 2>&1 | sed -n 's/.* \([0-9.]*\)MB\/s.*/\1/p')
		[ -n "$out" ] || { echo "-"; return; }
		best=${best:-$out}
		# keep the highest of the repeats, the slow ones are interference
		best=$(awk -v a="$best" -v b="$out" 'BEGIN{print (b>a)?b:a}')
		i=$((i + 1))
	done
	echo "$best"
}

# sizes that do not depend on the run
sizes() {
	local d=$1 extra=$2 text state
	$CC $CFLAGS $extra -I"$d/src" -DSLZ_VERSION='"zdec-perf"' -c "$d/src/uslz.c" -o "$tmp/u.o" 2>/dev/null &&
		text=$(size -A "$tmp/u.o" 2>/dev/null | awk '/^\.text/{print $2; exit}')
	printf '#include <stdio.h>\n#include "slz.h"\nint main(void){printf("%%zu",sizeof(struct uslz_stream));return 0;}\n' > "$tmp/sz.c"
	$CC -O1 $extra -I"$d/src" -o "$tmp/sz" "$tmp/sz.c" 2>/dev/null && state=$("$tmp/sz")
	echo "${text:--} ${state:--}"
}

row() { printf '%-34s %9s %9s %8s %8s\n' "$1" "$2" "$3" "$4" "$5"; }

if [ "$MODE" = full ] || [ "$MODE" = commits ]; then
	echo "=== every commit of $BASE..$TIP, built the same way ==="
	row "commit" "slz MB/s" "gzip MB/s" "text" "state"
	for c in $(git rev-list --reverse "$BASE" -n 1) $(git rev-list --reverse "$BASE..$TIP"); do
		d="$tmp/c"; rm -rf "$d"; mkdir -p "$d"
		git archive "$c" | tar -x -C "$d" || continue
		subj=$(git log -1 --format=%s "$c" | cut -c1-32)
		[ "$c" = "$(git rev-parse "$BASE")" ] && subj="[base] $subj"
		if build "$d" "$tmp/zdec"; then
			set -- $(sizes "$d" "")
			row "$subj" "$(speed "$tmp/zdec" "$SLZ")" "$(speed "$tmp/zdec" "$GZ")" "$1" "$2"
		else
			row "$subj" "build failed" "" "" ""
			head -3 "$tmp/cc.log" >&2
		fi
	done
	echo
fi

if [ "$MODE" = full ] || [ "$MODE" = options ]; then
	d="$tmp/tip"; rm -rf "$d"; mkdir -p "$d"
	git archive "$TIP" | tar -x -C "$d"
	echo "=== build options, all at $TIP ==="
	row "option" "slz MB/s" "gzip MB/s" "text" "state"
	# the two huffman lookup table widths, as in the table in slz.h
	oldifs=$IFS; IFS='|'
	for w in $WIDTHS; do
		IFS=$oldifs
		want widths || { IFS='|'; continue; }
		set -- $w
		o="-DUSLZ_FAST_BITS=$1 -DUSLZ_FAST_DBITS=$2"
		# they are #defined unconditionally, a -D only warns, so patch them
		sed -i "s/^#define USLZ_FAST_BITS .*/#define USLZ_FAST_BITS $1/;
		        s/^#define USLZ_FAST_DBITS .*/#define USLZ_FAST_DBITS $2/" "$d/src/slz.h"
		if build "$d" "$tmp/zdec"; then
			set -- $(sizes "$d" "")
			row "USLZ_FAST_BITS/DBITS = $w" "$(speed "$tmp/zdec" "$SLZ")" "$(speed "$tmp/zdec" "$GZ")" "$1" "$2"
		fi
		IFS='|'
	done
	IFS=$oldifs
	# whatever the defaults are, put the tip's own file back for the rest
	git archive "$TIP" src/slz.h | tar -x -C "$d"

	# the prefetch sites, when a version of the sources has them
	for p in $PREFETCH; do
		want prefetch || continue
		grep -q USLZ_PREFETCH "$d/src/uslz.c" || continue
		if build "$d" "$tmp/zdec" "-DUSLZ_PREFETCH=$p"; then
			set -- $(sizes "$d" "-DUSLZ_PREFETCH=$p")
			row "USLZ_PREFETCH = $p" "$(speed "$tmp/zdec" "$SLZ")" "$(speed "$tmp/zdec" "$GZ")" "$1" "$2"
		fi
	done

fi
