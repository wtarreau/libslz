/* uslz decompression micro-benchmark: decodes an in-memory copy of the
 * whole input file <rounds> times, feeding it in 8kB chunks and draining
 * the output without writing it, so that the measurement only covers the
 * decompression code and not the read()/write() syscalls.
 *
 * build: gcc -O3 -Isrc -o ubench tests/ubench.c src/slz_common.o src/uslz.o
 * usage: ubench <file> [rounds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "slz.h"

#define IN_CHUNK 8192

int main(int argc, char **argv)
{
	unsigned char obuf[32768];
	struct uslz_stream state;
	unsigned char *file;
	long file_len = 0, alloc;
	unsigned long total = 0;
	int rounds = argc > 2 ? atoi(argv[2]) : 3;
	int r, fd;
	struct timespec t0, t1;
	uint32_t crc = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <file> [rounds]\n", argv[0]);
		return 1;
	}

	fd = open(argv[1], 0);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	alloc = 1 << 20;
	file = malloc(alloc);
	while (1) {
		long ret;

		if (file_len == alloc) {
			alloc *= 2;
			file = realloc(file, alloc);
		}
		ret = read(fd, file + file_len, alloc - file_len);
		if (ret <= 0)
			break;
		file_len += ret;
	}
	close(fd);
	fprintf(stderr, "%s: %ld bytes, %d rounds\n", argv[1], file_len, rounds);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (r = 0; r < rounds; r++) {
		long ofs = 0;

		if (!uslz_init(&state, obuf, sizeof(obuf)))
			return 1;

		while (ofs < file_len) {
			const unsigned char *in = file + ofs;
			long ilen = file_len - ofs;
			int done = 0;

			if (ilen > IN_CHUNK)
				ilen = IN_CHUNK;
			ofs += ilen;

			while (!done) {
				unsigned char *dec;
				long dlen, consumed;
				enum uslz_decode_ret ret;

				ret = uslz_decode(&state, in, ilen, &dec, &dlen,
				                  &consumed, &crc);
				switch (ret) {
				case USLZ_DECODE_SUCCESS:
					total += dlen;
					done = 1;
					ofs = file_len;
					break;
				case USLZ_DECODE_OUT_OF_DATA:
					total += dlen;
					done = 1;
					break;
				case USLZ_DECODE_OUT_OF_SPACE:
					total += dlen;
					in += consumed;
					ilen -= consumed;
					break;
				default:
					fprintf(stderr, "error (%d)\n", ret);
					return 1;
				}
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "%lu bytes out, crc %08x, %.4f s/round\n", total, crc,
	        ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) / rounds);
	return 0;
}
