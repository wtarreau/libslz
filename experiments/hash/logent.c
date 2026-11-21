/* per-bit entropy of the words in a hash log written by the logging slz copy
 * (records of 3 uint32: word, offset, kind 0=lookup 1=insert 2=probe).
 * usage: logent <log>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"); uint32_t r[3]; double ones[32] = {0}, n = 0, kind[3] = {0};
	int b;
	while (fread(r, sizeof(r), 1, f) == 1) {
		kind[r[2]]++; n++;
		for (b = 0; b < 32; b++) ones[b] += (r[0] >> b) & 1;
	}
	printf("%s: %.0f words (lookup %.1f%% insert %.1f%% probe %.1f%%)\n", argv[1], n,
	       100 * kind[0] / n, 100 * kind[1] / n, 100 * kind[2] / n);
	for (b = 31; b >= 0; b--) {
		double q = ones[b] / n, H = (q <= 0 || q >= 1) ? 0 : -(q * log2(q) + (1 - q) * log2(1 - q));
		printf("%c%.2f", (b % 8 == 7) ? '|' : ' ', H);
	}
	printf("|\n");
	return 0;
}
