/* uslz decoder test driver: decodes stdin to stdout using a caller-chosen
 * output ring size and input chunk size, so that the state machine can be
 * exercised with arbitrary interruption points.
 *
 * It is built and driven by tests/uslztest.sh.
 *
 * usage: uslztest [ring_size] [in_chunk] [format]
 *
 * <format> selects uslz_init_fmt() instead of uslz_init(): 1 = raw deflate,
 * 2 = gzip, 3 = zlib. Anything else, or absent, means autodetection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "slz.h"

int main(int argc, char **argv)
{
	static unsigned char obuf[1 << 20];
	static unsigned char inbuf[8 << 20];
	struct uslz_stream state;
	long ring = argc > 1 ? atol(argv[1]) : 32768;
	long chunk = argc > 2 ? atol(argv[2]) : 8192;
	int fmt = argc > 3 ? atoi(argv[3]) : 0;
	long in_len = 0, ofs = 0;
	int complete = 0, ret;
	uint32_t crc = 0;

	if (ring > (long)sizeof(obuf) || chunk > (long)sizeof(inbuf))
		return 2;

	while (1) {
		long ret;

		if (in_len == (long)sizeof(inbuf)) {
			/* refuse rather than silently decode a truncated
			 * input, which would look like a decoder failure.
			 */
			fprintf(stderr, "input larger than %zu bytes\n", sizeof(inbuf));
			return 2;
		}
		ret = read(0, inbuf + in_len, sizeof(inbuf) - in_len);
		if (ret <= 0)
			break;
		in_len += ret;
	}

	if (fmt == 1)
		ret = uslz_init_fmt(&state, obuf, ring, SLZ_FMT_DEFLATE);
	else if (fmt == 2)
		ret = uslz_init_fmt(&state, obuf, ring, SLZ_FMT_GZIP);
	else if (fmt == 3)
		ret = uslz_init_fmt(&state, obuf, ring, SLZ_FMT_ZLIB);
	else
		ret = uslz_init(&state, obuf, ring);

	if (!ret) {
		fprintf(stderr, "init failed\n");
		return 2;
	}

	while (ofs < in_len) {
		const unsigned char *in = inbuf + ofs;
		long ilen = in_len - ofs;
		int done = 0;

		if (ilen > chunk)
			ilen = chunk;
		ofs += ilen;

		while (!done) {
			unsigned char *dec;
			long dlen, consumed;
			enum uslz_decode_ret ret;

			ret = uslz_decode(&state, in, ilen, &dec, &dlen, &consumed, &crc);
			switch (ret) {
			case USLZ_DECODE_SUCCESS:
			case USLZ_DECODE_OUT_OF_DATA:
			case USLZ_DECODE_OUT_OF_SPACE:
				while (dlen) {
					long w = write(1, dec, dlen);

					if (w <= 0)
						return 2;
					dec += w;
					dlen -= w;
				}
				if (ret == USLZ_DECODE_SUCCESS) {
					/* The stream is complete as far as the
					 * data fed so far goes. A gzip file is a
					 * series of members though, so keep
					 * feeding while there is input left
					 * instead of stopping here, otherwise
					 * only the first member is decoded.
					 */
					complete = 1;
					done = 1;
					break;
				}
				if (ret == USLZ_DECODE_OUT_OF_DATA) {
					complete = 0;
					done = 1;
					break;
				}
				in += consumed;
				ilen -= consumed;
				break;
			default:
				fprintf(stderr, "error (%d)\n", ret);
				return 1;
			}
		}
	}

	if (!complete) {
		fprintf(stderr, "truncated\n");
		return 1;
	}
	fprintf(stderr, "crc=%08x\n", crc);
	return 0;
}
