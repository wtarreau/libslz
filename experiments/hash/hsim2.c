/* Like hsim, but replays a hash log from the logging slz copy (records of 3
 * uint32: word, offset, kind 0=lookup 1=insert 2=probe): lookups count a hit
 * when their bucket holds the same word within 32768 bytes, then all kinds
 * update the bucket. Only every <stride>th chunk of 1M records is replayed.
 * usage: hsim2 <bits> <stride> <log> < candidates   (same format as hsim)
 * prints: <hits> <lookups> <candidate>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
static inline uint32_t op(uint32_t x, uint32_t y, int o) { return o == 0 ? x + y : o == 1 ? x - y : x ^ y; }
int main(int argc, char **argv)
{
	int bits = atoi(argv[1]), stride = atoi(argv[2]), fd = open(argv[3], O_RDONLY);
	struct stat st; const uint32_t *rec; long n, i;
	uint64_t *tab = malloc(sizeof(uint64_t) << bits);
	char line[256];
	fstat(fd, &st); n = st.st_size / 12;
	rec = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	while (fgets(line, sizeof(line), stdin)) {
		int fam, o1, o2; unsigned long p, q; long hits = 0, look = 0;
		if (sscanf(line, "%d %li %lu %d %d", &fam, (long *)&p, &q, &o1, &o2) != 5) continue;
		memset(tab, 0, sizeof(uint64_t) << bits);
		for (i = 0; i < n; i++) {
			uint32_t a, h, pos;
			if ((i >> 20) % stride) { i |= (1 << 20) - 1; continue; }
			a = rec[3 * i]; pos = rec[3 * i + 1] + 1;
			switch (fam) {
			case 0: h = op(op(a << p, a << q, o1), a, o2) >> (32 - bits); break;
			case 1: h = op(op(a, a >> p, o1), a >> q, o2) & ((1u << bits) - 1); break;
			case 2: h = (a * (uint32_t)p) >> (32 - bits); break;
			case 3: h = ((a ^ (a >> p)) * 0x9E3779B1u) >> (32 - bits); break;
			default: h = op(op(a, a >> p, o1), a << q, o2) >> (32 - bits); break;
			}
			if (rec[3 * i + 2] != 1) {
				look++;
				if ((uint32_t)tab[h] == a && tab[h] >> 32 && pos - (uint32_t)(tab[h] >> 32) <= 32768)
					hits++;
			}
			tab[h] = ((uint64_t)pos << 32) | a;
		}
		printf("%ld %ld %s", hits, look, line);
		fflush(stdout);
	}
	return 0;
}
