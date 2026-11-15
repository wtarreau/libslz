/* evtest: reliability test of the SLZ_CT_EVENT content type and of the flush:
 * random chunk sizes (mostly small, down to 1 byte), random flushes (one,
 * two in a row or none), checking after each flush that zlib can deliver
 * everything up to there, and that the whole stream inflates back to the
 * input. Random levels (0 never uses the ring), formats, and content types
 * (event 2/3 of the time, generic or text otherwise).
 *
 * usage: evtest <file> <seed> <iterations> [maxchunk]
 *   WRAP=1    start the positions so that they wrap at 2^32 mid-file (no
 *             gzip then, its trailer holds the faked length)
 *   DETACH=1  randomly remove the ring for some chunks, which must drop
 *             the history
 * build: gcc -O2 -Isrc evtest.c src/slz.c src/slz_common.c -lz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "slz.h"

static unsigned int rnd(unsigned int *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

int main(int argc, char **argv)
{
	FILE *f;
	unsigned char *in, *out, *dec;
	long ilen, outsz;
	unsigned int seed, it, iters, maxchunk;
	int fails = 0;
	int wrap = !!getenv("WRAP"), detach = !!getenv("DETACH");

	if (argc < 4)
		return fprintf(stderr, "usage: %s file seed iterations [maxchunk]\n", argv[0]), 1;
	f = fopen(argv[1], "rb");
	if (!f)
		return perror(argv[1]), 1;
	fseek(f, 0, SEEK_END);
	ilen = ftell(f);
	fseek(f, 0, SEEK_SET);
	in = malloc(ilen + 1);
	if (fread(in, 1, ilen, f) != (size_t)ilen)
		return perror("read"), 1;
	fclose(f);
	seed = atoi(argv[2]);
	iters = atoi(argv[3]);
	maxchunk = argc > 4 ? atoi(argv[4]) : 2048;
	outsz = ilen * 2 + 1048576;
	out = malloc(outsz);
	dec = malloc(ilen + 1);

	for (it = 0; it < iters; it++) {
		struct slz_stream strm;
		void *ring = calloc(1, slz_ring_size);
		unsigned int s = seed + it * 7919;
		int level = rnd(&s) % 5;
		/* gzip's trailer holds strm->ilen, which WRAP fakes */
		int fmt = wrap ? SLZ_FMT_ZLIB + rnd(&s) % 2 : SLZ_FMT_GZIP + rnd(&s) % 3;
		long olen = 0, pos = 0, fed = 0;
		z_stream z;
		int ret, ct;

		slz_init(&strm, level, fmt);
		ct = rnd(&s) % 6;
		ct = ct < 4 ? SLZ_CT_EVENT : ct == 4 ? SLZ_CT_GENERIC : SLZ_CT_TEXT;
		slz_set_content_type(&strm, ct);
		strm.ring = ring;
		if (wrap)
			strm.ilen = 0u - (unsigned int)(ilen / 2);
		memset(&z, 0, sizeof(z));
		inflateInit2(&z, fmt == SLZ_FMT_GZIP ? 31 : fmt == SLZ_FMT_ZLIB ? 15 : -15);
		z.next_out = dec;
		z.avail_out = ilen + 1;

		while (pos < ilen) {
			long c = (rnd(&s) & 3) ? 1 + rnd(&s) % 300 : 1 + rnd(&s) % maxchunk;
			int fl = rnd(&s) % 3;

			if (c > ilen - pos)
				c = ilen - pos;
			strm.ring = (detach && !(rnd(&s) % 16)) ? NULL : ring;
			olen += slz_encode(&strm, out + olen, in + pos, c, 1);
			pos += c;
			if (fl == 2)
				continue;
			olen += slz_flush(&strm, out + olen);
			if (fl == 1)
				olen += slz_flush(&strm, out + olen);
			z.next_in = out + fed;
			z.avail_in = olen - fed;
			ret = inflate(&z, Z_SYNC_FLUSH);
			fed = olen - z.avail_in;
			if ((ret != Z_OK && ret != Z_BUF_ERROR) || z.total_out != (unsigned long)pos ||
			    memcmp(dec, in, pos)) {
				fprintf(stderr, "FAIL it=%u level=%d fmt=%d ct=%d flush=%d: %lu/%ld delivered ret=%d\n",
				        it, level, fmt, ct, fl, z.total_out, pos, ret);
				fails++;
				goto next;
			}
		}
		olen += slz_finish(&strm, out + olen);
		z.next_in = out + fed;
		z.avail_in = olen - fed;
		ret = inflate(&z, Z_FINISH);
		if (ret != Z_STREAM_END || z.total_out != (unsigned long)ilen || memcmp(dec, in, ilen)) {
			fprintf(stderr, "FAIL it=%u level=%d fmt=%d ct=%d at finish: ret=%d out=%lu/%ld\n",
			        it, level, fmt, ct, ret, z.total_out, ilen);
			fails++;
		}
	next:
		inflateEnd(&z);
		free(ring);
	}
	free(in);
	free(out);
	free(dec);
	printf("%s: %u runs, %d failures\n", argv[1], iters, fails);
	return fails != 0;
}
