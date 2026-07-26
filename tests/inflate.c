/*
 * Regression test for the API promise that the output never exceeds the input
 * by more than 5 bytes per 65535 input bytes, plus 2 bytes for the final
 * BFINAL+EOB.
 *
 * Two families of inputs are checked:
 *
 *  1) crafted patterns made of <run> literals >= 144 followed by 4 bytes
 *     copied from 8 bytes earlier. Literals 144 to 255 cost 9 bits in fixed
 *     huffman mode instead of the 8 bits they cost in a stored block, and the
 *     short reference used to cost nothing more than a reset of the counter
 *     that measures this waste, so the encoder never switched back to stored
 *     blocks and the output kept growing. With <run> just under the 52 bits
 *     threshold this used to produce 1073161 bytes out of 1048576 (+2.34%)
 *     where at most 1048663 are allowed.
 *
 *  2) small random inputs of the same nature, which used to exceed the
 *     promise for lengths as small as 47 bytes when the last literals of a
 *     block were charged the cost of a round trip to a stored block that was
 *     not needed since nothing follows them.
 *
 * Both compress at level 1 in raw deflate, where the accounting is exact
 * (gzip and zlib only add their fixed header and trailer on top).
 *
 * The encoder does not meet the promise to the byte: a reference emitted
 * between two stored blocks splits them and adds a block header nobody paid
 * for, and the debt tracking that stops the growth tolerates SLZ_MAX_DEBT
 * (200) bits before it kicks in. The test therefore allows a small excess
 * over the promise and always prints the worst case it found, so that a
 * regression shows up as a number even when it stays under the limit. What
 * matters is that the excess stays bounded: with the bug, run=51 costs 24498
 * bytes on 1 MB, 68481 on 4 MB and 147356 on 8 MB, while it is 14 bytes at
 * every size once fixed.
 *
 * Note that this only checks the size bound. A fix for it can easily cost
 * compression ratio instead, which this test cannot see, so the ratio must be
 * verified separately on a real corpus, e.g. "zenc -t -v < silesia.tar".
 *
 * Usage: inflate [runs]
 *   0 (default) tests everything, otherwise only the crafted pattern is
 *   tested with the given number of literals between the references.
 *
 * Build: cc -O2 -I../src -o inflate inflate.c ../src/slz.c ../src/slz_common.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slz.h"

#define ILEN (1 << 20)

/* Bytes by which the output is allowed to exceed the documented maximum, see
 * the comment above: a fixed part for the debt (200 bits) and the block that
 * detects it, plus one byte per 64 kB for the block splitting, which is not
 * a regression (v1.3.0 produces the exact same numbers there).  The worst
 * case observed is 51 bytes on 1 MB, where 80 are allowed.
 */
static long tolerance(long ilen)
{
	return 57 + ilen / 65536;
}

static unsigned char in[ILEN], out[2 * ILEN];
static unsigned int rnd_state;

static unsigned int rnd(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return (rnd_state >> 8) & 0xffffff;
}

/* maximum output size the API promises for <ilen> input bytes when the block
 * is the last one: 5 bytes per 65535 input bytes for the stored blocks, plus
 * 2 bytes for the final BFINAL+EOB.
 */
static long max_output(long ilen)
{
	return ilen + 5 * ((ilen + 65534) / 65535) + 2;
}

/* compresses <ilen> bytes from <in> in a single call, returns the output size */
static long compress_one(long ilen)
{
	struct slz_stream strm;
	long olen;

	slz_init(&strm, 1, SLZ_FMT_DEFLATE);
	olen = slz_encode(&strm, out, in, ilen, 0);
	olen += slz_finish(&strm, out + olen);
	return olen;
}

/* fills <ilen> bytes of <in> with <run> literals >= 144 followed by 4 bytes
 * copied from 8 bytes earlier, so that a cheap reference is found regularly.
 */
static void make_pattern(long ilen, long run)
{
	long i, j;

	rnd_state = 12345;
	for (i = 0; i < 16 && i < ilen; i++)
		in[i] = 144 + (rnd() % 112);

	/* a first long match so that the encoder switches to fixed huffman */
	for (; i < 32 && i < ilen; i++)
		in[i] = in[i - 16];

	while (i < ilen) {
		for (j = 0; j < run && i < ilen; j++, i++)
			in[i] = 144 + (rnd() % 112);
		for (j = 0; j < 4 && i < ilen; j++, i++)
			in[i] = in[i - 8];
	}
}

static long worst;      /* largest excess over the documented maximum */
static long worst_ilen;
static long worst_run;

static int check_pattern(long ilen, long run, int verbose)
{
	long olen, max;

	make_pattern(ilen, run);
	olen = compress_one(ilen);
	max = max_output(ilen);

	if (olen - max > worst) {
		worst = olen - max;
		worst_ilen = ilen;
		worst_run = run;
	}

	if (verbose || olen > max + tolerance(ilen))
		printf("%s run=%-4ld ilen=%-9ld olen=%-9ld max=%-9ld (%+ld bytes, %+.2f%%)\n",
		       (olen > max + tolerance(ilen)) ? "FAIL" : "ok  ", run, ilen, olen, max,
		       olen - max, (olen - ilen) * 100.0 / ilen);
	return olen > max + tolerance(ilen);
}

int main(int argc, char **argv)
{
	long run, ilen, iter;
	int fails = 0;

	if (argc > 1 && atol(argv[1]))
		return check_pattern(ILEN, atol(argv[1]), 1);

	/* 1) crafted patterns, at several sizes and run lengths */
	for (run = 4; run <= 120; run++) {
		fails += check_pattern(ILEN, run, 0);
		fails += check_pattern(65536, run, 0);
		fails += check_pattern(8192, run, 0);
		fails += check_pattern(1000, run, 0);
	}
	printf("crafted patterns: %d failure(s), worst %+ld bytes over the promise"
	       " (ilen=%ld run=%ld)\n", fails, worst, worst_ilen, worst_run);

	/* 2) small random inputs made of the same kind of data */
	rnd_state = 1;
	for (iter = 0; iter < 200000; iter++) {
		long i;
		long olen, max;

		ilen = 8 + rnd() % 200;
		for (i = 0; i < ilen; i++)
			in[i] = (rnd() % 16) ? 144 + rnd() % 112 : in[i > 8 ? i - 8 : 0];

		olen = compress_one(ilen);
		max = max_output(ilen);
		if (olen - max > worst) {
			worst = olen - max;
			worst_ilen = ilen;
			worst_run = -1;
		}
		if (olen > max + tolerance(ilen)) {
			if (fails++ < 10)
				printf("FAIL random ilen=%ld olen=%ld max=%ld (+%ld)\n",
				       ilen, olen, max, olen - max);
		}
	}
	printf("%d failure(s) in total, worst %+ld bytes over the promise\n",
	       fails, worst);
	return !!fails;
}
