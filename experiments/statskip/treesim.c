/* treesim: given an input file and the matches an encoder emitted for it (a
 * SLZ_TRACE trace, 'M' records with the distance code's bit count in bits
 * 16-20 of the length), computes the size the stream would have under
 * several ways of encoding the literals between these matches: with the
 * fixed tree (F), the text tree (T), or as stored blocks (S). The matches
 * stay the same for all of them, only the block decisions change.
 *
 * Costs, in bits: a literal costs its code length in the tree of its block,
 * or 8 in a stored block (plus 42 per 65535 bytes); a match costs its length
 * code (fixed or text) plus its distance code. Leaving a block costs its EOB
 * (7 for F, 11 for T); opening one costs 3 + the tree's header (0 for F, 624
 * for T) or 42 for a stored block (type, alignment, LEN, NLEN).
 *
 * Usage: treesim <file> <trace>
 *
 * Build: gcc -O2 -I../../src -o treesim treesim.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "tables.h"

struct rec { uint32_t type, len; uint64_t pos; };

enum { F = 0, T = 1, S = 2, NST = 3 };

static const int eob[NST]  = { 7, 11, 0 };
static const int open_[NST] = { 3, 3 + TEXT_HDR_BITS, 42 };
#define INF 1e18
#define SLIT (8.0 + 42.0 / 65535.0)   /* stored literal, incl. block splits */

static unsigned char *in;
static size_t ilen;
static struct rec *m;     /* matches only, sorted */
static size_t nm;
static int litc[2][256], lenc[2][259];

static double trans(int a, int b)
{
	if (a == b)
		return 0;
	return eob[a] + open_[b];
}

/* exact optimum over all block sequences, restricted to states in <mask> */
static double dp(int mask)
{
	double c[NST], n[NST];
	size_t pos = 0, i = 0;
	int s, p;

	for (s = 0; s < NST; s++)
		c[s] = (mask & (1 << s)) ? open_[s] : INF;

	while (pos < ilen) {
		int is_m = i < nm && m[i].pos == pos;
		for (s = 0; s < NST; s++) {
			double best = INF;
			if (!(mask & (1 << s)) || (is_m && s == S)) {
				n[s] = INF;
				continue;
			}
			for (p = 0; p < NST; p++)
				if (c[p] + trans(p, s) < best)
					best = c[p] + trans(p, s);
			if (is_m)
				n[s] = best + lenc[s][m[i].len & 0xffff] + ((m[i].len >> 16) & 0x1f);
			else
				n[s] = best + (s == S ? SLIT : litc[s][in[pos]]);
		}
		memcpy(c, n, sizeof(c));
		if (is_m)
			pos += m[i++].len & 0xffff;
		else
			pos++;
	}
	for (s = 0; s < NST; s++)
		c[s] += eob[s];
	return c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
}

/* Greedy policies, deciding once per literal run (at the next match):
 *   split = 0: the whole run in the current tree, or the whole run stored;
 *   split = 1: also a prefix in the current tree up to the minimum of its
 *              running excess over 8 bits, and the rest stored.
 * The tree used after the run (for the match) is <want>, given by the
 * trigger:
 *   trig = 0: always <fixed_tree> (no switch);
 *   trig = 1: full accounting: CUSUM of the bits the other tree would have
 *             saved on literals and matches, switch when above <thr> bits;
 *   trig = 2: bit 7 only: CUSUM in half-bits, +3 per literal < 0x80, -4 per
 *             literal >= 0x80 towards T, the opposite towards F, switch
 *             when above <thr> half-bits;
 *   trig = 3: bit 7 (as 2) to enter T, full accounting (as 1) to leave it,
 *             which is nearly free in T since <lit_excess> is already
 *             known there and the fixed cost is 8 + (c >= 144).
 * <thr2> is the threshold to leave T for F (bits or half-bits).
 * Returns the total in bits, and the number of tree switches in <sw>.
 */
static double greedy(int start, int split, int trig, long thr, long thr2, long *sw)
{
	int cur = start;   /* tree of the current huffman block (F or T) */
	int want = start;
	double tot = open_[start], cus = 0;
	size_t pos = 0, i = 0;

	*sw = 0;
	while (pos <= ilen) {
		size_t end = i < nm ? m[i].pos : ilen;
		size_t n = end - pos, k, kbest = 0;
		double sum = 0, mn = 0;
		int last = (i >= nm);

		/* literals [pos, end) in <cur>, and the trigger */
		for (k = 0; k < n; k++) {
			unsigned char ch = in[pos + k];
			sum += litc[cur][ch] - 8;
			if (sum < mn) {
				mn = sum;
				kbest = k + 1;
			}
			if (trig == 1 || (trig == 3 && cur == T)) {
				int d = cur == F ? litc[F][ch] - litc[T][ch] : litc[T][ch] - litc[F][ch];
				cus += d;
			} else if (trig == 2 || trig == 3) {
				int d = (ch & 0x80) ? -4 : 3;
				cus += cur == F ? d : -d;
			}
			if (cus < 0)
				cus = 0;
		}
		if (trig && cus > (cur == F ? thr : thr2)) {
			want = !cur;
			cus = 0;
		} else
			want = cur;

		/* options for this run (and the match after it) */
		{
			double whole = sum + 8.0 * n + (want != cur ? trans(cur, want) : 0);
			double reopen = last ? 0 : open_[want];
			double stored = eob[cur] + 42 + SLIT * n + reopen;
			double sp = split ? mn + 8.0 * kbest + eob[cur] + 42 + SLIT * (n - kbest) + reopen : INF;
			double best = whole;
			int stor = 0;

			if (stored < best) { best = stored; stor = 1; }
			if (sp < best) { best = sp; stor = 1; }
			if (n == 0) { best = want != cur ? trans(cur, want) : 0; stor = 0; }
			if (last && !stor)
				best += eob[want];
			tot += best;
		}
		if (want != cur) {
			(*sw)++;
			cur = want;
		}
		if (last)
			break;
		{
			int ml = m[i].len & 0xffff;
			tot += lenc[cur][ml] + ((m[i].len >> 16) & 0x1f);
			if (trig == 1 || (trig == 3 && cur == T)) {
				cus += cur == F ? lenc[F][ml] - lenc[T][ml] : lenc[T][ml] - lenc[F][ml];
				if (cus < 0)
					cus = 0;
			}
			pos = end + ml;
			i++;
		}
	}
	return tot;
}

int main(int argc, char **argv)
{
	FILE *f;
	struct rec *r;
	size_t nr, j;
	long sw, t;
	double ref;
	int c;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <file> <trace>\n", argv[0]);
		return 1;
	}
	f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END); ilen = ftell(f); rewind(f);
	in = malloc(ilen);
	if (fread(in, 1, ilen, f) != ilen) return 1;
	fclose(f);
	f = fopen(argv[2], "rb");
	fseek(f, 0, SEEK_END); nr = ftell(f) / sizeof(*r); rewind(f);
	r = malloc(nr * sizeof(*r));
	if (fread(r, sizeof(*r), nr, f) != nr) return 1;
	fclose(f);
	m = malloc(nr * sizeof(*m));
	for (j = nm = 0; j < nr; j++)
		if (r[j].type == 'M')
			m[nm++] = r[j];

	for (c = 0; c < 256; c++) {
		litc[F][c] = fixed_huff[c] & 15;
		litc[T][c] = text_huff[c] & 15;
	}
	for (c = 3; c < 259; c++) {
		lenc[F][c] = len_fh[c] >> 16;
		lenc[T][c] = text_len[c] >> 16;
	}

	ref = greedy(T, 0, 0, 0, 0, &sw) / 8;
	printf("%-34s %12s %8s %8s\n", "policy", "bytes", "vs T", "switches");
#define P(name, v, s) do { double v_ = (v); printf("%-34s %12.0f %+7.2f%% %8ld\n", name, v_ / 8, 100.0 * (v_ / 8 / ref - 1), (long)(s)); } while (0)
	P("T, whole run or stored", greedy(T, 0, 0, 0, 0, &sw), sw);
	P("T, split at min excess", greedy(T, 1, 0, 0, 0, &sw), sw);
	P("F, whole run or stored", greedy(F, 0, 0, 0, 0, &sw), sw);
	P("F, split at min excess", greedy(F, 1, 0, 0, 0, &sw), sw);
	for (t = 1; t <= 4; t *= 2) {
		char name[64];
		snprintf(name, sizeof(name), "F/T split, full acct %ld/%d", t * 640, 64);
		P(name, greedy(F, 1, 1, t * 640, 64, &sw), sw);
	}
	for (t = 1; t <= 4; t *= 2) {
		char name[64];
		snprintf(name, sizeof(name), "F/T split, bit 7 %ld/%d", t * 1280, 128);
		P(name, greedy(F, 1, 2, t * 1280, 128, &sw), sw);
		snprintf(name, sizeof(name), "F/T split, bit 7 %ld/%d", t * 1280, 512);
		P(name, greedy(F, 1, 2, t * 1280, 512, &sw), sw);
	}
	for (t = 1; t <= 4; t *= 2) {
		char name[64];
		snprintf(name, sizeof(name), "F/T split, bit 7 %ld / acct %d", t * 1280, 640);
		P(name, greedy(F, 1, 3, t * 1280, 640, &sw), sw);
		snprintf(name, sizeof(name), "F/T split, bit 7 %ld / acct %d", t * 1280, 2560);
		P(name, greedy(F, 1, 3, t * 1280, 2560, &sw), sw);
	}
	P("optimum F+T (no stored)", dp((1 << F) | (1 << T)), 0);
	P("optimum T+S", dp((1 << T) | (1 << S)), 0);
	P("optimum F+S", dp((1 << F) | (1 << S)), 0);
	P("optimum F+T+S", dp(7), 0);
	return 0;
}
