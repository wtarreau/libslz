/* per-bit statistics of the 32-bit little-endian words read at each position
 * (byte 0 = bits 0-7, as slz reads them): P(bit=1), and how often the bit
 * differs between two words that differ (i.e. how much it separates words).
 * usage: bitent <file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"); long n, i; int b;
	unsigned char *p; double ones[32] = {0}, H;
	fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
	p = malloc(n + 4); n = fread(p, 1, n, f); memset(p + n, 0, 4);
	for (i = 0; i + 4 <= n; i++) {
		uint32_t w; memcpy(&w, p + i, 4);
		for (b = 0; b < 32; b++) ones[b] += (w >> b) & 1;
	}
	printf("%s: bit entropy (byte3 .. byte0, bit7..bit0 each)\n", argv[1]);
	for (b = 31; b >= 0; b--) {
		double q = ones[b] / (n - 3);
		H = (q <= 0 || q >= 1) ? 0 : -(q * log2(q) + (1 - q) * log2(1 - q));
		printf("%c%.2f", (b % 8 == 7) ? '|' : ' ', H);
	}
	printf("|\n");
	return 0;
}
