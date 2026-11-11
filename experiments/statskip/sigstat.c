/* sigstat: how well do cheap byte statistics tell incompressible chunks from
 * compressible ones?
 *
 * Usage: sigstat <data> <trace-noskip> [gap [from to]]
 *
 * <trace-noskip> is the fd 3 output of a zenc built with -DSLZ_TRACE
 * -DSLZ_LITERAL_SKIP=0 run on <data> (level 1 or 2, they are identical without
 * the skip). It gives every match the encoder finds when it scans every byte.
 * A 64-byte chunk is labelled "incompressible" when it lies entirely within a
 * run of literals of at least <gap> bytes (default 1024) of that trace, and
 * "compressible" when at least one of its bytes is covered by a match. The
 * others (inside short literal runs) are counted apart.
 *
 * For each signal and threshold, prints the share of incompressible bytes
 * that a skip relying on it would pass over ("recall"), and the share of all
 * matched bytes that lie in chunks it would wrongly pass over ("lost").
 * <from> and <to> restrict the statistics to that range of the input (e.g.
 * one member of a tar file), the runs being still measured on all of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

struct rec { uint32_t type, len; uint64_t pos; };

static unsigned char *load(const char *name, size_t *len)
{
	FILE *f = fopen(name, "rb");
	unsigned char *p;

	if (!f) { perror(name); exit(1); }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(*len + 64);
	if (fread(p, 1, *len, f) != *len) { perror(name); exit(1); }
	fclose(f);
	return p;
}

#define NSIG 10
static const char *signame[NSIG] = {
	"ewma256 |mean-128| <=",   /* Willy's avg, in units of 1/256 */
	"ewma256 mad >=",          /* EWMA of |x - avg| */
	"ewma32 |mean-128| <=",
	"ewma32 mad >=",
	"chunk |mean-128| <=",
	"chunk sd >=",
	"chunk eq1+eq2+eq4 <=",    /* bytes equal to the one 1, 2 or 4 before */
	"chunk distinct >=",
	"chunk hibit |n-32| <=",
	"combo mean+sd+eq",        /* chunk mean <= 28, sd >= 60, eq <= 3 */
};

int main(int argc, char **argv)
{
	size_t len, tlen, nrec, i, c, nchunks;
	unsigned char *d, *tr;
	struct rec *r;
	uint8_t *cov;     /* 1 = covered by a match */
	uint32_t *run;    /* per byte: length of the literal run containing it, 0 if covered */
	long gap = 1024;
	size_t from = 0, to = 0;
	double tot_cov = 0, tot_inc = 0, tot_mid = 0, tot_comp = 0;
	/* per signal, histogram of the value over incompressible chunks (bytes)
	 * and over compressible chunks (matched bytes in them)
	 */
	static double hinc[NSIG][300], hcov[NSIG][300], hmid[NSIG][300];

	if (argc < 3) { fprintf(stderr, "usage: %s data trace [gap]\n", argv[0]); return 1; }
	if (argc > 3) gap = atol(argv[3]);
	if (argc > 5) { from = atol(argv[4]) / 64; to = atol(argv[5]) / 64; }
	d = load(argv[1], &len);
	tr = load(argv[2], &tlen);
	r = (struct rec *)tr; nrec = tlen / sizeof(*r);
	cov = calloc(len, 1);
	run = calloc(len, sizeof(*run));
	for (i = 0; i < nrec; i++) {
		if (r[i].type != 'M') continue;
		if (r[i].pos + (r[i].len & 0xffff) > len) { fprintf(stderr, "bad record\n"); return 1; }
		memset(cov + r[i].pos, 1, r[i].len & 0xffff);
	}
	for (i = 0; i < len; ) {
		size_t j = i;
		if (cov[i]) { i++; continue; }
		while (j < len && !cov[j]) j++;
		for (size_t k = i; k < j; k++) run[k] = j - i > 0xffffffff ? 0xffffffff : j - i;
		i = j;
	}

	nchunks = len / 64;
	double a256 = 128 * 256.0, m256 = 64 * 256.0, a32 = 128 * 32.0, m32 = 64 * 32.0;
	/* integer versions as Willy wrote them would behave the same; doubles
	 * keep this readable.
	 */
	for (c = 0; c < nchunks; c++) {
		const unsigned char *p = d + c * 64;
		int ccov = 0, inc = 1, distinct = 0, hib = 0, eq = 0;
		uint8_t seen[256] = { 0 };
		double s = 0, s2 = 0;
		int v[NSIG];

		for (i = 0; i < 64; i++) {
			unsigned x = p[i];
			ccov += cov[c * 64 + i];
			if (run[c * 64 + i] < (uint32_t)gap) inc = 0;
			a256 += x - a256 / 256; m256 += fabs(x - a256 / 256) - m256 / 256;
			a32 += x - a32 / 32;    m32 += fabs(x - a32 / 32) - m32 / 32;
			s += x; s2 += (double)x * x;
			if (!seen[x]) { seen[x] = 1; distinct++; }
			hib += x >> 7;
			if (c * 64 + i >= 4)
				eq += (x == p[(long)i - 1]) + (x == p[(long)i - 2]) + (x == p[(long)i - 4]);
		}
		double mean = s / 64, sd = sqrt(s2 / 64 - mean * mean);
		v[0] = (int)fabs(a256 / 256 - 127.5);
		v[1] = (int)(m256 / 256);
		v[2] = (int)fabs(a32 / 32 - 127.5);
		v[3] = (int)(m32 / 32);
		v[4] = (int)fabs(mean - 127.5);
		v[5] = (int)sd;
		v[6] = eq;
		v[7] = distinct;
		v[8] = abs(hib - 32);
		v[9] = (fabs(mean - 127.5) <= 28 && sd >= 60 && eq <= 3) ? 1 : 0;
		if (to && (c < from || c >= to))
			continue;
		for (i = 0; i < NSIG; i++) {
			int x = v[i] < 0 ? 0 : v[i] > 299 ? 299 : v[i];
			if (inc) hinc[i][x] += 64;
			else if (ccov) hcov[i][x] += ccov;
			else hmid[i][x] += 64;
		}
		if (inc) tot_inc += 64; else if (ccov) tot_comp += 64; else tot_mid += 64;
		tot_cov += ccov;
	}

	printf("# %s: %zu bytes, %.1f%% matched, incompressible (runs >= %ld): %.1f%%, "
	       "compressible chunks: %.1f%%, other: %.1f%%\n", argv[1], len,
	       100 * tot_cov / len, gap, 100 * tot_inc / len, 100 * tot_comp / len, 100 * tot_mid / len);
	if (to)
		len = (to - from) * 64, printf("# range %zu-%zu: %.1f%% matched, incompressible %.1f%%\n",
		                              from * 64, to * 64, 100 * tot_cov / len, 100 * tot_inc / len);
	for (i = 0; i < NSIG; i++) {
		/* "<=" signals pass when value <= t, ">=" when value >= t */
		int le = strstr(signame[i], "<=") != NULL;
		static const int th[NSIG][6] = {
			{ 4, 8, 12, 16, 24, 32 },
			{ 64, 60, 56, 52, 48, 40 },
			{ 4, 8, 12, 16, 24, 32 },
			{ 64, 60, 56, 52, 48, 40 },
			{ 8, 16, 20, 28, 36, 48 },
			{ 72, 68, 64, 60, 56, 50 },
			{ 0, 1, 2, 3, 5, 8 },
			{ 56, 54, 52, 50, 48, 44 },
			{ 4, 6, 8, 12, 16, 24 },
			{ 1, 1, 1, 1, 1, 1 },
		};
		printf("  %-24s", signame[i]);
		for (int k = 0; k < (i == 9 ? 1 : 6); k++) {
			double a = 0, b = 0, m = 0;
			for (int x = 0; x < 300; x++) {
				int pass = (i == 9) ? x == 1 : le ? x <= th[i][k] : x >= th[i][k];
				if (pass) { a += hinc[i][x]; b += hcov[i][x]; m += hmid[i][x]; }
			}
			printf(" %3d:%5.1f/%5.2f/%4.1f", i == 9 ? 1 : th[i][k], tot_inc ? 100 * a / tot_inc : 0,
			       100 * b / tot_cov, tot_mid ? 100 * m / tot_mid : 0);
		}
		printf("\n");
	}
	printf("  (threshold:recall%%/lost%%/other%%, recall = incompressible bytes passed,\n"
	       "   lost = matched bytes in passed chunks, other = short-run literal chunks passed)\n");
	return 0;
}
