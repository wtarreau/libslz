#!/bin/bash
# Correctness matrix for the uslz decoder: several stream formats and
# encoders, decoded with a range of output ring sizes and input chunk
# sizes, checked against the original data.
cd "$(dirname "$0")/.." || exit 1
# prefer a tmpfs, the test writes a few hundred MB of temporary streams
for d in /dev/shm "$TMPDIR" /tmp; do
	[ -n "$d" ] && [ -w "$d" ] && T="$d/uslztest.$$" && break
done
mkdir -p "$T" || exit 1
trap 'rm -rf "$T"' EXIT

# USR_CFLAGS must match what src/uslz.o was built with: USLZ_FAST_BITS and
# USLZ_FAST_DBITS change the size of struct uslz_stream, which this driver
# instantiates, so a mismatch silently corrupts memory rather than failing to
# build. Use the same value as the make invocation, e.g.
#   make USR_CFLAGS=-DUSLZ_FAST_BITS=0 && USR_CFLAGS=-DUSLZ_FAST_BITS=0 tests/uslztest.sh
gcc -O2 -Wall -Isrc $USR_CFLAGS -o "$T/uslztest" tests/uslztest.c src/slz_common.o src/uslz.o || exit 1

fail=0 total=0

# reference inputs: a small text file, a large one, incompressible data,
# highly repetitive data (long matches), an empty file and a 1-byte file.
mkdir -p "$T/ref"
cp tests/index.html "$T/ref/text"
cat tests/index.html tests/daniels.html tests/index.html > "$T/ref/text2"
cp tests/noncomp.bin "$T/ref/noncomp"
perl -e 'print "ab" x 300000' > "$T/ref/rep2"
perl -e 'print "A" x 500000' > "$T/ref/rep1"
perl -e 'for (1..40000) { print "The quick brown fox jumps over the lazy dog.\n" }' > "$T/ref/rep44"
: > "$T/ref/empty"
printf 'x' > "$T/ref/one"
# seeded, *not* /dev/urandom: incompressible data makes the encoders emit
# stored blocks and unusual code lengths, so it finds real bugs, but it has
# to be reproducible or the pass count moves between runs and a regression
# cannot be told from a different draw.
python3 -c 'import random,sys; random.seed(20260725); sys.stdout.buffer.write(random.randbytes(40000))' > "$T/ref/rand40k"
cat tests/index.html tests/noncomp.bin tests/daniels.html > "$T/ref/mixed"

for r in "$T"/ref/*; do
	n=${r##*/}
	# gzip: fast and best; zenc: slz static huffman; python: zlib and raw
	# deflate containers, and a gzip made of stored blocks only. The latter
	# is built by hand because not every gzip(1) accepts -0, and a silently
	# missing file would just be skipped below.
	gzip -1 -c  < "$r" > "$T/$n.gz1"
	gzip -9 -c  < "$r" > "$T/$n.gz9"
	python3 -c 'import sys,zlib,struct
d=sys.stdin.buffer.read()
c=zlib.compressobj(0,zlib.DEFLATED,-15)
body=c.compress(d)+c.flush()
sys.stdout.buffer.write(b"\x1f\x8b\x08\0\0\0\0\0\0\x03"+body+struct.pack("<II",zlib.crc32(d)&0xffffffff,len(d)&0xffffffff))' < "$r" > "$T/$n.gz0"
	./zenc       < "$r" > "$T/$n.slz" 2>/dev/null
	python3 -c 'import sys,zlib; sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(),6))' < "$r" > "$T/$n.zlib"
	python3 -c 'import sys,zlib
c=zlib.compressobj(6,zlib.DEFLATED,-15)
sys.stdout.buffer.write(c.compress(sys.stdin.buffer.read())+c.flush())' < "$r" > "$T/$n.raw"
	# gzip with every optional header field set: FEXTRA, FNAME, FCOMMENT
	# and FHCRC. gzip(1) itself never emits those from a pipe, so without
	# this the header parser is only ever exercised on its 10-byte form.
	python3 -c 'import sys,zlib,struct
d=sys.stdin.buffer.read()
c=zlib.compressobj(9,zlib.DEFLATED,-15)
body=c.compress(d)+c.flush()
h=b"\x1f\x8b\x08"+bytes([0x02|0x04|0x08|0x10])+b"\0\0\0\0\0\x03"
h+=struct.pack("<H",6)+b"ABCDEF"
h+=b"name.txt\0"+b"a comment\0"
h+=struct.pack("<H",zlib.crc32(h)&0xffff)
sys.stdout.buffer.write(h+body+struct.pack("<II",zlib.crc32(d)&0xffffffff,len(d)&0xffffffff))' < "$r" > "$T/$n.gzh"

	for f in "$T/$n".{gz0,gz1,gz9,gzh,slz,zlib,raw}; do
		[ -s "$f" ] || continue
		for ring in 32768 32769 40000 65536 131072; do
			for chunk in 1 3 61 8192 1000000; do
				total=$((total + 1))
				"$T/uslztest" $ring $chunk < "$f" > "$T/out" 2>"$T/err"
				if [ $? -ne 0 ] || ! cmp -s "$r" "$T/out"; then
					echo "FAIL ${f##*/} ring=$ring chunk=$chunk: $(cat "$T/err")"
					fail=$((fail + 1))
				fi
			done
		done
	done
done

# the same streams again, but telling the decoder which envelope to expect
# through uslz_init_fmt() rather than letting it detect one. For raw deflate
# this is the only reliable way, see the trap stream below.
for n in text noncomp rep44 rand40k; do
	for spec in "raw 1" "gz9 2" "zlib 3"; do
		set -- $spec
		f="$T/$n.$1"
		[ -s "$f" ] || continue
		for chunk in 1 7 8192; do
			total=$((total + 1))
			"$T/uslztest" 32768 $chunk $2 < "$f" > "$T/out" 2>"$T/err"
			if [ $? -ne 0 ] || ! cmp -s "$T/ref/$n" "$T/out"; then
				echo "FAIL forced-fmt $n.$1 chunk=$chunk: $(cat "$T/err")"
				fail=$((fail + 1))
			fi
		done
	done
done

# announcing an envelope the stream does not carry must be refused, not
# silently decoded as something else
for spec in "text.raw 2" "text.raw 3" "text.gz9 3" "text.zlib 2"; do
	set -- $spec
	total=$((total + 1))
	"$T/uslztest" 32768 8192 $2 < "$T/$1" > /dev/null 2>&1
	[ $? -eq 1 ] || { echo "FAIL forced-fmt $1 as $2 was not refused"; fail=$((fail + 1)); }
done

# A raw deflate stream whose first two bytes happen to form a valid zlib
# header: autodetection cannot get this right, forcing the format must.
python3 -c '
import sys
payload = bytes(range(29))
out = bytearray([0x08])                  # BFINAL=0 BTYPE=00, padding bit 3 set
out += len(payload).to_bytes(2, "little")
out += (~len(payload) & 0xFFFF).to_bytes(2, "little")
out += payload
out += bytes([0x03, 0x00])               # final fixed block, immediate EOB
sys.stdout.buffer.write(bytes(out))
open("'"$T"'/trap.ref", "wb").write(payload)' > "$T/trap.deflate"
total=$((total + 1))
"$T/uslztest" 32768 8192 1 < "$T/trap.deflate" > "$T/out" 2>/dev/null
if [ $? -ne 0 ] || ! cmp -s "$T/trap.ref" "$T/out"; then
	echo "FAIL zlib-looking raw deflate stream not decoded with a forced format"
	fail=$((fail + 1))
fi

# multi-member gzip: concatenated members are a single valid gzip stream
# (that is what "gzip -c a b > ab.gz" and most log rotators produce), and
# the decoder is expected to return the concatenation of all of them.
cat "$T/text.gz9" "$T/rep44.gz9" > "$T/multi.gz"
cat "$T/ref/text" "$T/ref/rep44" > "$T/multi.ref"
for chunk in 1 61 8192 1000000; do
	total=$((total + 1))
	"$T/uslztest" 32768 $chunk < "$T/multi.gz" > "$T/out" 2>"$T/err"
	if [ $? -ne 0 ] || ! cmp -s "$T/multi.ref" "$T/out"; then
		echo "FAIL multi-member gzip chunk=$chunk: $(cat "$T/err")"
		fail=$((fail + 1))
	fi
done

# truncated and corrupted streams must be rejected, not crash
for f in "$T"/text.gz9 "$T"/text.slz; do
	head -c 200 "$f" > "$T/trunc"
	"$T/uslztest" 32768 7 < "$T/trunc" > /dev/null 2>&1
	[ $? -eq 1 ] || { echo "FAIL truncated ${f##*/} not rejected"; fail=$((fail + 1)); }
	total=$((total + 1))
	for ofs in 30 100 300 900; do
		python3 -c "
import sys
d=bytearray(open('$f','rb').read())
if len(d) > $ofs: d[$ofs] ^= 0x5a
sys.stdout.buffer.write(d)" > "$T/corrupt"
		"$T/uslztest" 32768 7 < "$T/corrupt" > /dev/null 2>&1
		[ $? -eq 2 ] && { echo "FAIL corrupt ${f##*/}@$ofs crashed"; fail=$((fail + 1)); }
		total=$((total + 1))
	done
done

echo "$((total - fail))/$total passed"
[ $fail -eq 0 ]
