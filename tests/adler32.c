/*
 * Regression test for slz_adler32_block(), which computes the checksum used by
 * the zlib format (RFC1950). It is compared against slz_adler32_by1(), the
 * straightforward implementation from the RFC that does a modulus per byte.
 *
 * slz_adler32_block() avoids that modulus by folding the sums once per block
 * of 4096 bytes, so the accumulators grow large in between: s2 may reach
 * 65520 + 4096 * (65520 + 255 * 4096) = 2408052720. That fits in 32 bits but
 * not in a *signed* 32-bit one, and the accumulators used to be declared as
 * long, which is 32-bit on i386 and friends. The subsequent shifts were then
 * done on a negative value and the top half of the checksum came out wrong:
 *
 *   $ gcc -m32 ... && ./adler32
 *   MISMATCH len=13312 fill=0xff block=072dcefe by1=08efcefe
 *
 * so a 32-bit build produced zlib streams that no decompressor accepted, and
 * rejected valid ones on the decoding side, as soon as the data contained
 * enough large byte values. 64-bit builds were unaffected.
 *
 * Usage: adler32
 * Build: cc -O2 -I../src -o adler32 adler32.c ../src/slz_common.c
 *        (also build it with -m32 if the toolchain supports it)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slz.h"

#define BUFSIZE (1 << 20)

static unsigned char buf[BUFSIZE];
static unsigned int rnd_state = 1;

static unsigned int rnd(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return (rnd_state >> 8) & 0xffffff;
}

int main(void)
{
	long len, pos, i, tested = 0, bad = 0;
	int fill;

	printf("sizeof(long) = %d\n", (int)sizeof(long));

	/* 1) constant fills, which is where the accumulators grow fastest */
	for (fill = 0; fill < 256; fill += 15) {
		memset(buf, fill, sizeof(buf));
		for (len = 0; len <= BUFSIZE; len += (len < 32768) ? 512 : 65536) {
			uint32_t a = slz_adler32_block(1, buf, len);
			uint32_t b = slz_adler32_by1(1, buf, len);

			tested++;
			if (a != b && bad++ < 10)
				printf("MISMATCH len=%ld fill=0x%02x block=%08x by1=%08x\n",
				       len, fill, a, b);
		}
	}

	/* 2) random data, random lengths, random starting values */
	for (i = 0; i < BUFSIZE; i++)
		buf[i] = rnd();

	for (i = 0; i < 20000; i++) {
		uint32_t crc = (i & 1) ? 1 : (rnd() << 8) + (rnd() & 0xff);
		uint32_t a, b;

		len = rnd() % BUFSIZE;
		a = slz_adler32_block(crc, buf, len);
		b = slz_adler32_by1(crc, buf, len);
		tested++;
		if (a != b && bad++ < 10)
			printf("MISMATCH random len=%ld crc=%08x block=%08x by1=%08x\n",
			       len, crc, a, b);
	}

	/* 3) same data fed in random chunks must give the same result */
	memset(buf, 0xff, sizeof(buf));
	for (i = 0; i < 2000; i++) {
		uint32_t a = 1, b;

		len = rnd() % BUFSIZE;
		b = slz_adler32_by1(1, buf, len);
		for (pos = 0; pos < len; ) {
			long l = 1 + rnd() % 9000;

			if (l > len - pos)
				l = len - pos;
			a = slz_adler32_block(a, buf + pos, l);
			pos += l;
		}
		tested++;
		if (a != b && bad++ < 10)
			printf("MISMATCH chunked len=%ld block=%08x by1=%08x\n", len, a, b);
	}

	printf("%ld checks, %ld mismatch(es)\n", tested, bad);
	return !!bad;
}
