/*
 * Copyright (C) 2026 HAProxy Technologies
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "slz.h"

int main(void)
{
	unsigned char obuf[32768];
	unsigned char inbuf[8192];
	struct uslz_stream state;
	long decoded_total = 0;

	if (!uslz_init(&state, obuf, sizeof(obuf))) {
		fprintf(stderr, "failed to init uslz state\n");
		exit(1);
	}

	while (1) {
		unsigned char *input;
		unsigned char *decoded;
		long decoded_len;
		long consumed;
		enum uslz_decode_ret decode_ret;
		int rret, wret;
		uint32_t crc;

		rret = read(0, inbuf, sizeof(inbuf));
		if (rret <= 0) {
			fprintf(stderr, "read error\n");
			exit(1);
		}
		input = inbuf;
 redo:
		decode_ret = uslz_decode(&state, input, rret, &decoded, &decoded_len, &consumed, &crc);
		switch(decode_ret) {
			case USLZ_DECODE_SUCCESS:
			case USLZ_DECODE_OUT_OF_SPACE:
			case USLZ_DECODE_OUT_OF_DATA:
			{
				while (decoded_len) {
					wret = write(1, decoded, decoded_len);
					if (wret < 0) {
						fprintf(stderr, "write error\n");
						exit(1);
					}
					decoded += wret;
					decoded_total += wret;
					decoded_len -= wret;
				}

				if (decode_ret == USLZ_DECODE_SUCCESS) {
					/* printf mainly useful for debug purposes, let's be
					 * silent by default
					 */
					//fprintf(stderr, "crc = %08x, size = %lu\n", crc, decoded_total);
					exit(0);
				}
				/* out of space */
				input += consumed;
				rret -= consumed;
				if (decode_ret == USLZ_DECODE_OUT_OF_DATA)
					continue;
				goto redo;
			}
			default:
				fprintf(stderr, "error (%d)\n", decode_ret);
				exit(1);
				break;
		}
	}
	return 0;
}
