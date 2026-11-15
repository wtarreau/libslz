/* evbench: cost per event of compressing many concurrent SSE streams, as a
 * proxy would: <nstr> independent streams, each with its own slz stream,
 * event context and receive buffer, all visited in a random order at each
 * round, one event each. The event is first copied into the stream's receive
 * buffer (as recv() would), then compressed and flushed into a shared output
 * buffer. With many streams, nothing of a stream's context is left in the
 * caches when it gets its next event.
 *
 * usage: evbench <file> <nstr> <rounds> <mode> [seed]
 *   mode: event   slz_rfc1951_encode_event() + slz_rfc1951_eflush()
 *         today   slz_rfc1951_encode() level 1 + slz_rfc1951_flush(), no history
 *         copy    only the copy into the receive buffer (the loop's own cost)
 * prints: mode nstr events ns/event out-bytes ratio
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "slz.h"

struct strm {
	struct slz_stream z;
	struct slz_ev_ctx *ctx;
	unsigned char *rx;
};

static unsigned int rnd(unsigned int *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

int main(int argc, char **argv)
{
	FILE *f;
	unsigned char *raw, *in, *out;
	long rlen, ilen = 0, i, nev = 0, *evs, *evl;
	int nstr, rounds, mode, r, s;
	unsigned int seed;
	struct strm **st;
	int *perm;
	struct timespec t0, t1;
	unsigned long long olen = 0, tin = 0, events = 0;
	double ns;
	int lf = !!getenv("LF"); /* LF=1: one event per line as is (logs) */
	long maxl = 0;

	if (argc < 5)
		return fprintf(stderr, "usage: %s file nstr rounds event|today|copy [seed]\n", argv[0]), 1;
	f = fopen(argv[1], "rb");
	if (!f)
		return perror(argv[1]), 1;
	fseek(f, 0, SEEK_END);
	rlen = ftell(f);
	fseek(f, 0, SEEK_SET);
	raw = malloc(rlen);
	if (fread(raw, 1, rlen, f) != (size_t)rlen)
		return perror("read"), 1;
	fclose(f);
	nstr = atoi(argv[2]);
	rounds = atoi(argv[3]);
	mode = !strcmp(argv[4], "event") ? 0 : !strcmp(argv[4], "today") ? 1 : 2;
	seed = argc > 5 ? atoi(argv[5]) : 1;

	/* events: each line + "\n\n" */
	in = malloc(rlen * 2 + 2);
	evs = malloc(sizeof(*evs) * (rlen + 1));
	evl = malloc(sizeof(*evl) * (rlen + 1));
	for (i = 0; i < rlen; i++) {
		if (i == 0 || raw[i - 1] == '\n')
			evs[nev++] = ilen;
		in[ilen++] = raw[i];
		if (raw[i] == '\n') {
			if (!lf)
				in[ilen++] = '\n';
			evl[nev - 1] = ilen - evs[nev - 1];
		}
	}
	if (raw[rlen - 1] != '\n')
		evl[nev - 1] = ilen - evs[nev - 1];

	for (i = 0; i < nev; i++)
		if (evl[i] > maxl)
			maxl = evl[i];

	st = malloc(sizeof(*st) * nstr);
	perm = malloc(sizeof(*perm) * nstr);
	for (s = 0; s < nstr; s++) {
		st[s] = malloc(sizeof(**st));
		slz_rfc1951_init(&st[s]->z, 1);
		st[s]->ctx = aligned_alloc(64, sizeof(*st[s]->ctx));
		slz_ev_init(st[s]->ctx);
		st[s]->rx = malloc(maxl);
		perm[s] = s;
	}
	out = malloc(1 << 20);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (r = 0; r < rounds; r++) {
		/* new random order at each round */
		for (s = nstr - 1; s > 0; s--) {
			int j = rnd(&seed) % (s + 1), t = perm[s];

			perm[s] = perm[j];
			perm[j] = t;
		}
		for (i = 0; i < nstr; i++) {
			struct strm *p = st[perm[i]];
			long e = (perm[i] * 37L + r) % nev;
			long l = evl[e];
			long o;

			memcpy(p->rx, in + evs[e], l);
			if (mode == 0) {
				o = slz_rfc1951_encode_event(&p->z, out, p->rx, l, 1, p->ctx);
				o += slz_rfc1951_eflush(&p->z, out + o);
			} else if (mode == 1) {
				o = slz_rfc1951_encode(&p->z, out, p->rx, l, 1);
				o += slz_rfc1951_flush(&p->z, out + o);
			} else
				o = p->rx[l - 1];
			olen += o;
			tin += l;
			events++;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
	printf("%s %d %llu %.1f %llu %.2f%%\n", argv[4], nstr, events, ns / events, olen,
	       mode == 2 ? 0.0 : olen * 100.0 / tin);
	return 0;
}
