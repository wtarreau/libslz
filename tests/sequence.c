/*
 * Fuzzes random sequences of init/encode/flush/finish for the three formats
 * and both levels, and checks that:
 *
 *   - the whole output decodes back to the concatenation of the inputs;
 *   - flush() and finish() never emit more than what they document;
 *   - the total output never exceeds the documented maximum for the stream.
 *     Note that a single encode() call may exceed its own share by a few
 *     bytes, since up to 31 bits are retained in the queue from one call to
 *     the next; only the sum over the stream is bounded.
 *
 * It notably covers the sequences that are easy to get wrong: a flush before
 * any data has been sent, several flushes in a row, empty encode() calls, and
 * a flush after the last encode() call. That last one used to produce a
 * corrupt gzip or zlib stream: encode() with <more> cleared may leave the
 * stream in SLZ_ST_LAST, i.e. with a block whose BFINAL bit is already set,
 * and flush() then terminated that block *and* emitted an empty one, which
 * lands past the end of the deflate stream and shifts the trailer that
 * finish() appends. The data decoded fine but the checksum did not match:
 *
 *   inflate: ret=-3 (incorrect data check), 5954 sequences out of 300000
 *
 * Raw deflate was not affected since it has no trailer to shift.
 *
 * Usage: sequence
 * Build: cc -O2 -I../src -o sequence sequence.c ../src/slz.c ../src/slz_common.c -lz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "slz.h"

static unsigned char src[1 << 18], in[1 << 18], out[1 << 19], chk[1 << 19];
static unsigned int rnd_state = 1;

static unsigned int rnd(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return (rnd_state >> 8) & 0xffffff;
}

/* documented worst cases, header included */
static const int max_flush[3]  = { 20, 12, 10 };  /* gzip, zlib, deflate */
static const int max_finish[3] = { 24, 12,  6 };  /* incl. 10/2/0 of header */

int main(void)
{
	static const int fmts[3] = { SLZ_FMT_GZIP, SLZ_FMT_ZLIB, SLZ_FMT_DEFLATE };
	long iter, i;
	int errors = 0, over = 0;
	long worst_flush = 0, worst_finish = 0, worst_total = 0;

	for (i = 0; i < (long)sizeof(src); i++) {
		int t = (i / 1000) % 4;

		src[i] = (t == 0) ? rnd() :                    /* random */
		         (t == 1) ? "abcdefgh0123"[i % 12] :   /* repetitive */
		         (t == 2) ? 32 + rnd() % 64 :          /* text */
		                    144 + rnd() % 112;         /* 9-bit literals */
	}

	for (iter = 0; iter < 300000; iter++) {
		struct slz_stream strm;
		int f = rnd() % 3, lvl = rnd() & 1;
		long olen = 0, ilen = 0, omax = 0;
		int ops = 1 + rnd() % 8;
		int op, nflush = 0;
		z_stream z;
		int ret;

		slz_init(&strm, lvl, fmts[f]);

		/* a flush or a finish may come first, before any data */
		if (!(rnd() % 8)) {
			long n = slz_flush(&strm, out + olen);

			if (n > max_flush[f]) {
				printf("FLUSH TOO LARGE (initial): fmt=%d %ld > %d\n", f, n, max_flush[f]);
				over++;
			}
			if (n > worst_flush)
				worst_flush = n;
			olen += n;
			nflush++;
		}

		for (op = 0; op < ops; op++) {
			long l = rnd() % 3 ? (rnd() % 3000) : 0;   /* sometimes empty */
			long ofs = rnd() % (sizeof(src) - 3000);
			int more = (op < ops - 1);

			if (ilen + l > (long)sizeof(in))
				break;
			memcpy(in + ilen, src + ofs, l);
			olen += slz_encode(&strm, out + olen, in + ilen, l, more);
			omax += l + 5 * ((l + 65534) / 65535) + (more ? 0 : 2);
			ilen += l;

			/* random flushes, sometimes several in a row */
			while (!(rnd() % 3)) {
				long n = slz_flush(&strm, out + olen);

				if (n > max_flush[f]) {
					printf("FLUSH TOO LARGE: fmt=%d %ld > %d\n", f, n, max_flush[f]);
					over++;
				}
				if (n > worst_flush)
					worst_flush = n;
				olen += n;
				nflush++;
			}
		}

		{
			long n = slz_finish(&strm, out + olen);

			if (n > max_finish[f]) {
				printf("FINISH TOO LARGE: fmt=%d %ld > %d\n", f, n, max_finish[f]);
				over++;
			}
			if (n > worst_finish)
				worst_finish = n;
			olen += n;
		}

		/* the flushes and the envelope are budgeted separately: each
		 * flush may add its documented maximum, and so may finish().
		 */
		omax += nflush * max_flush[f] + max_finish[f];
		if (olen - omax > worst_total)
			worst_total = olen - omax;
		if (olen > omax) {
			printf("TOTAL TOO LARGE: fmt=%d ilen=%ld olen=%ld max=%ld\n", f, ilen, olen, omax);
			over++;
		}

		/* now decode and compare */
		memset(&z, 0, sizeof(z));
		inflateInit2(&z, (f == 0) ? 47 : (f == 1) ? 15 : -15);
		z.next_in = out;
		z.avail_in = olen;
		z.next_out = chk;
		z.avail_out = sizeof(chk);
		ret = inflate(&z, Z_FINISH);
		if (ret != Z_STREAM_END || (long)z.total_out != ilen ||
		    (ilen && memcmp(chk, in, ilen))) {
			if (errors++ < 10)
				printf("MISMATCH fmt=%d lvl=%d ops=%d ilen=%ld olen=%ld ret=%d out=%ld (%s)\n",
				       f, lvl, ops, ilen, olen, ret, (long)z.total_out, z.msg ? z.msg : "");
		}
		inflateEnd(&z);
	}
	printf("300000 sequences: %d mismatches, %d bound violations "
	       "(worst flush=%ld finish=%ld, worst total %+ld over the promise)\n",
	       errors, over, worst_flush, worst_finish, worst_total);
	return errors || over;
}
