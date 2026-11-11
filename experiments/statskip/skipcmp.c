/* skipcmp: compares the skips of an encoder build against the matches found
 * when scanning every byte.
 *
 * Usage: skipcmp <size> <trace-noskip> <trace-skip> [gap [from to]]
 *
 * Both traces come from zenc built with -DSLZ_TRACE (the first one also with
 * -DSLZ_LITERAL_SKIP=0), see sigstat.c. Prints, over the whole input or the
 * range <from>-<to>:
 *   - skipped: bytes passed over by skip steps (not looked up);
 *   - lost: bytes matched by the no-skip encoder that were skipped;
 *   - inc: bytes in literal runs of at least <gap> bytes (default 1024) of
 *     the no-skip trace, i.e. what an ideal skip would pass over;
 *   - inc scanned: bytes of those runs that were not skipped, i.e. looked up
 *     one by one by the main loop;
 *   - overshoot: skipped bytes past the end of such a run, i.e. after the
 *     first byte the no-skip encoder matched again, histogram per episode;
 *   - steps/hits: number of skip steps, and of those stopped by a probe hit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct rec { uint32_t type, len; uint64_t pos; };

static struct rec *load(const char *name, size_t *n)
{
	FILE *f = fopen(name, "rb");
	size_t len;
	void *p;

	if (!f) { perror(name); exit(1); }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(len + 1);
	if (fread(p, 1, len, f) != len) { perror(name); exit(1); }
	fclose(f);
	*n = len / sizeof(struct rec);
	return p;
}

int main(int argc, char **argv)
{
	size_t len, n0, n1, i, from = 0, to;
	struct rec *t0, *t1;
	uint8_t *cov, *skp;
	uint32_t *run;
	long gap = 1024;
	double skipped = 0, lost = 0, inc = 0, incscan = 0, cov0 = 0;
	long steps = 0, hits = 0;

	if (argc < 4) { fprintf(stderr, "usage: %s size t0 t1 [gap [from to]]\n", argv[0]); return 1; }
	len = atol(argv[1]);
	to = len;
	t0 = load(argv[2], &n0);
	t1 = load(argv[3], &n1);
	if (argc > 4) gap = atol(argv[4]);
	if (argc > 6) { from = atol(argv[5]); to = atol(argv[6]); }

	cov = calloc(len, 1);
	skp = calloc(len, 1);
	run = calloc(len, sizeof(*run));
	for (i = 0; i < n0; i++)
		if (t0[i].type == 'M')
			memset(cov + t0[i].pos, 1, t0[i].len & 0xffff);
	for (i = 0; i < n1; i++) {
		if (t1[i].type != 'S' || t1[i].pos < from || t1[i].pos >= to)
			continue;
		uint32_t l = t1[i].len & 0x7fffffff;
		memset(skp + t1[i].pos, 1, l);
		steps++;
		hits += !!(t1[i].len & 0x80000000u);
	}
	for (i = 0; i < len; ) {
		size_t j = i;
		if (cov[i]) { i++; continue; }
		while (j < len && !cov[j]) j++;
		for (size_t k = i; k < j; k++) run[k] = j - i;
		i = j;
	}

	/* overshoot per episode: a maximal sequence of skipped bytes that ends
	 * on covered bytes; count the skipped bytes after the first covered one.
	 */
	long hist[8] = { 0 }; /* 0, 1-63, 64-255, 256-1023, 1k-4k, 4k-8k, 8k+ */
	double over = 0;
	for (i = from; i < to; ) {
		size_t j, k, o = 0;
		if (!skp[i]) { i++; continue; }
		for (j = i; j < to && skp[j]; j++)
			;
		for (k = i; k < j && !cov[k]; k++)
			;
		o = j - k; /* skipped after the run's end */
		over += o;
		hist[o == 0 ? 0 : o < 64 ? 1 : o < 256 ? 2 : o < 1024 ? 3 : o < 4096 ? 4 : o < 8192 ? 5 : 6]++;
		i = j;
	}

	for (i = from; i < to; i++) {
		cov0 += cov[i];
		skipped += skp[i];
		lost += skp[i] & cov[i];
		if (run[i] >= (uint32_t)gap) {
			inc++;
			incscan += !skp[i];
		}
	}
	double sz = to - from;
	printf("range %zu-%zu (%.1f MB): matched %.1f%%  skipped %.2f%%  lost %.3f%% of matched"
	       "  inc %.2f%%  inc scanned %.2f%% of inc  overshoot %.3f%%  steps %ld  hits %ld\n",
	       from, to, sz / 1e6, 100 * cov0 / sz, 100 * skipped / sz, cov0 ? 100 * lost / cov0 : 0,
	       100 * inc / sz, inc ? 100 * incscan / inc : 0, 100 * over / sz, steps, hits);
	printf("  overshoot per episode: 0:%ld 1-63:%ld 64-255:%ld 256-1k:%ld 1k-4k:%ld 4k-8k:%ld 8k+:%ld\n",
	       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
	return 0;
}
