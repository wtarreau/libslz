/*
 * Forces a match of every length from 4 to 258 at every distance sampled on
 * the boundaries of the distance codes, from 1 to 32768, and checks that zlib
 * decodes the result back to the original. This validates the two tables the
 * encoder emits references from, len_fh[] for the lengths and fh_dist_table[]
 * for the distances, including the entries that a normal corpus rarely reaches
 * (the longest matches and the furthest distances).
 *
 * It is worth running for the three variants that produce those tables
 * differently: the default build recomputes fh_dist_table[] at startup, and
 * -DPRECOMPUTE_TABLES uses the copy shipped in tables.h, which is what the
 * shared library is built with.
 *
 * Usage: codes
 * Build: cc -O2 -I../src -o codes codes.c ../src/slz.c ../src/slz_common.c -lz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "slz.h"

static unsigned char in[1 << 17], out[1 << 18], chk[1 << 18];
static unsigned int rs = 1;
static unsigned int rnd(void) { rs = rs * 1103515245 + 12345; return (rs >> 8) & 0xffffff; }

/* builds: <dist> random bytes, then a copy of <mlen> bytes taken <dist> back,
 * then a few random bytes so that the match is not at the very end.
 */
static long build(long dist, long mlen)
{
	long i, len = 0;

	rs = 1 + dist * 31 + mlen;
	for (i = 0; i < dist; i++)
		in[len++] = rnd();
	for (i = 0; i < mlen; i++, len++)
		in[len] = in[len - dist];
	for (i = 0; i < 8; i++)
		in[len++] = rnd();
	return len;
}

int main(void)
{
	long dists[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 16, 17, 24, 25, 32, 33, 48, 49, 64,
	                 65, 96, 97, 128, 129, 192, 193, 256, 257, 384, 385, 512, 513, 768, 769,
	                 1024, 1025, 1536, 1537, 2048, 2049, 3072, 3073, 4096, 4097, 6144, 6145,
	                 8192, 8193, 12288, 12289, 16384, 16385, 24576, 24577, 32768 };
	int nd = sizeof(dists) / sizeof(dists[0]);
	long d, mlen, tested = 0, bad = 0;

	for (d = 0; d < nd; d++) {
		for (mlen = 4; mlen <= 258; mlen++) {
			struct slz_stream strm;
			long len = build(dists[d], mlen), olen;
			z_stream z;
			int ret;

			slz_init(&strm, 1, SLZ_FMT_DEFLATE);
			olen = slz_encode(&strm, out, in, len, 0);
			olen += slz_finish(&strm, out + olen);

			memset(&z, 0, sizeof(z));
			inflateInit2(&z, -15);
			z.next_in = out; z.avail_in = olen;
			z.next_out = chk; z.avail_out = sizeof(chk);
			ret = inflate(&z, Z_FINISH);
			tested++;
			if (ret != Z_STREAM_END || (long)z.total_out != len || memcmp(chk, in, len)) {
				if (bad++ < 10)
					printf("FAIL dist=%ld mlen=%ld ilen=%ld olen=%ld ret=%d out=%ld (%s)\n",
					       dists[d], mlen, len, olen, ret, (long)z.total_out,
					       z.msg ? z.msg : "");
			}
			inflateEnd(&z);
		}
	}
	printf("%ld (distance, length) combinations tested, %ld failure(s)\n", tested, bad);
	return !!bad;
}
