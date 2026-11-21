/* Screens cheap hash functions by simulating slz's reference table: one entry
 * per bucket holding (word, pos); a position "hits" when its bucket holds the
 * same 32-bit word within 32768 bytes. More hits ~ more matches found.
 * Candidates are read from stdin, one per line: "<fam> <p> <q> <o1> <o2>"
 *   fam 0: ((a<<p) o1 (a<<q) o2 a) >> (32-bits)       top bits, sparse multiply
 *   fam 1: (a o1 (a>>p) o2 (a>>q)) & mask             low bits, right folds
 *   fam 2: a * <p as hex constant> >> (32-bits)        reference multiplies
 *   fam 3: ((a ^ (a>>p)) * 0x9E3779B1) >> (32-bits)   near-ideal reference
 *   fam 4: ((a o1 (a>>p)) o2 (a<<q)) >> (32-bits)      mixed fold then top bits
 * o: 0 '+', 1 '-', 2 '^'
 * usage: hsim <bits> <maxbytes-per-file> files... < candidates
 * prints: <hits per file...> <sum> <candidate>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static inline uint32_t op(uint32_t x, uint32_t y, int o) { return o == 0 ? x + y : o == 1 ? x - y : x ^ y; }
int main(int argc, char **argv)
{
	int bits = atoi(argv[1]), nf = argc - 3, k;
	long maxb = atol(argv[2]), len[16];
	unsigned char *buf[16];
	uint64_t *tab = malloc(sizeof(uint64_t) << bits);
	char line[256];
	for (k = 0; k < nf; k++) {
		FILE *f = fopen(argv[3 + k], "rb");
		buf[k] = malloc(maxb + 4);
		len[k] = fread(buf[k], 1, maxb, f); fclose(f);
	}
	while (fgets(line, sizeof(line), stdin)) {
		int fam, o1, o2; unsigned long p, q; long tot = 0;
		if (sscanf(line, "%d %li %lu %d %d", &fam, (long *)&p, &q, &o1, &o2) != 5) continue;
		for (k = 0; k < nf; k++) {
			long i, hits = 0; unsigned char *b = buf[k];
			memset(tab, 0, sizeof(uint64_t) << bits);
			for (i = 0; i + 4 <= len[k]; i++) {
				uint32_t a, h; memcpy(&a, b + i, 4);
				switch (fam) {
				case 0: h = op(op(a << p, a << q, o1), a, o2) >> (32 - bits); break;
				case 1: h = op(op(a, a >> p, o1), a >> q, o2) & ((1u << bits) - 1); break;
				case 2: h = (a * (uint32_t)p) >> (32 - bits); break;
				case 3: h = ((a ^ (a >> p)) * 0x9E3779B1u) >> (32 - bits); break;
				default: h = op(op(a, a >> p, o1), a << q, o2) >> (32 - bits); break;
				}
				if ((uint32_t)tab[h] == a && i - (long)(tab[h] >> 32) <= 32768 && tab[h] >> 32)
					hits++;
				tab[h] = ((uint64_t)(i + 1) << 32 | a); /* pos+1 so that 0 means empty */
			}
			printf("%ld ", hits); tot += hits;
		}
		printf("%ld %s", tot, line);
		fflush(stdout);
	}
	return 0;
}
