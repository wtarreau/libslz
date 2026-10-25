/*
 * Builds the huffman tree used for text contents (SLZ_CT_TEXT).
 *
 * The tree is described by its code lengths, one line per alphabet in a text
 * file: the 288 literal/length code lengths, then the 30 distance code
 * lengths. This tool has five modes:
 *
 *   mktext -c file.raw...      counts the symbols found in raw deflate
 *                              streams (e.g. "zenc -D"), and prints the
 *                              counts in the same format, one line per
 *                              alphabet.
 *
 *   mktext -t [-m max] [-l len syms] [-a a-b]... counts
 *                              turns counts into code lengths limited to
 *                              <max> bits (default 9) using package-merge.
 *                              Symbols the encoder may emit get a code even
 *                              if they were never seen. With -l, the
 *                              literals in <syms> (e.g. "0-8,11-31") form a
 *                              tail of rare symbols that all get <len> bits
 *                              (a few get len-1 to complete the tree), which
 *                              leaves shorter codes to the other ones and
 *                              costs little in the block header. With -a,
 *                              symbols a to b (288 and above for distances)
 *                              get the average of their counts first, hence
 *                              the same length, which is also cheap in the
 *                              header.
 *
 *   mktext -g lengths          emits the tables for src/tables.h: the codes
 *                              in the same formats as fixed_huff[] and
 *                              len_fh[], the dynamic block header that
 *                              declares the tree, and what is needed to
 *                              build text_dist_table[] from fh_dist_table[].
 *
 *   mktext -d lengths          emits text_dist_table[] for the precomputed
 *                              tables section of src/tables.h, in the same
 *                              format as fh_dist_table[].
 *
 *   mktext -e lengths counts   prints the number of bits these counts would
 *                              take with this tree and with the fixed one.
 *
 * Typical use, to train on a corpus then generate the tables:
 *
 *   for f in corpus/[*]; do zenc -D -1 < $f > $f.raw; done
 *   mktext -c corpus/[*].raw > counts
 *   mktext -t -m 11 counts > tools/text-tree.txt
 *   mktext -g tools/text-tree.txt
 *   mktext -d tools/text-tree.txt
 *
 * 11 bits is the longest code the length and distance table formats above can
 * hold, and literals may have up to 15 bits (text_huff[] is then emitted as
 * 32-bit). The tree in tools/text-tree.txt was trained this way at level 1 on
 * 2514 web files (957 HTML, 853 JS, 498 SVG, 104 JSON, 102 CSS, picked at
 * random, one in two of a local collection), to which the counts from the
 * first half of the Linux 7.0 sources tarball were added with 20% of the
 * weight, mostly so that tabs get a short code. Adding non-latin texts or
 * base64 to the training costs a little on ASCII and gains little on them.
 *
 * Other trees were evaluated with -e and kept out, as none was a better
 * tradeoff. A 15-bit tail for the control characters other than TAB and LF,
 * DEL and all bytes above it (-l 15 0-8,11-31,126-255) saves only 0.5 to 1%
 * on HTML, CSS and JS, but costs 7.6% on silesia.tar in text mode, since
 * binary contents mislabelled as text then pay 15 bits per byte. Trees
 * limited to 9 or 10 bits save 120 or 57 bits of header, but lose 5 or 1.5
 * points on web contents, which is paid back in less than 1 kB. Averaging
 * the rare letters V to Z and the first four distance codes (-a 86-90 -a
 * 288-291) changes nothing but the header (-14 bits). For reference, a tree
 * trained on silesia.tar itself would only save 14.2% on its level 1 symbols
 * where this one saves 10.7%, but a tree trained on each of its files would
 * save 20%: silesia.tar is made of very different contents, and is more of
 * an average than representative of any of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define NLIT  288
#define NDIST 30

static const int len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577 };
static const int dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* order of the code length code lengths in the block header */
static const int clen_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static void die(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

/***************************************************************************
 * deflate parser, only used to count symbols. Slow and simple, after puff.
 ***************************************************************************/

struct bitrd {
	const unsigned char *in;
	size_t len, pos;
	uint32_t bitbuf;
	int bitcnt;
};

static int getbits(struct bitrd *s, int need)
{
	uint32_t val = s->bitbuf;

	while (s->bitcnt < need) {
		if (s->pos >= s->len)
			die("truncated stream");
		val |= (uint32_t)s->in[s->pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return val & ((1U << need) - 1);
}

struct huffdec {
	short count[16];
	short symbol[NLIT];
};

static void build_dec(struct huffdec *h, const int *lens, int n)
{
	short offs[16];
	int i;

	memset(h->count, 0, sizeof(h->count));
	for (i = 0; i < n; i++)
		h->count[lens[i]]++;
	h->count[0] = 0;
	offs[1] = 0;
	for (i = 1; i < 15; i++)
		offs[i + 1] = offs[i] + h->count[i];
	for (i = 0; i < n; i++)
		if (lens[i])
			h->symbol[offs[lens[i]]++] = i;
}

static int decode(struct bitrd *s, const struct huffdec *h)
{
	int code = 0, first = 0, index = 0, len, count;

	for (len = 1; len < 16; len++) {
		code |= getbits(s, 1);
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	die("invalid huffman code");
	return -1;
}

static void count_stream(const unsigned char *in, size_t len,
                         unsigned long long *lc, unsigned long long *dc)
{
	struct bitrd s = { .in = in, .len = len };
	struct huffdec lit, dist;
	int lens[NLIT + NDIST];
	int last, type, i;

	do {
		last = getbits(&s, 1);
		type = getbits(&s, 2);
		if (type == 0) {
			unsigned l;

			s.bitbuf = 0;
			s.bitcnt = 0;
			if (s.pos + 4 > s.len)
				die("truncated stored block");
			l = in[s.pos] | (in[s.pos + 1] << 8);
			s.pos += 4 + l;
			continue;
		}
		else if (type == 1) {
			for (i = 0; i < 144; i++) lens[i] = 8;
			for (; i < 256; i++) lens[i] = 9;
			for (; i < 280; i++) lens[i] = 7;
			for (; i < NLIT; i++) lens[i] = 8;
			build_dec(&lit, lens, NLIT);
			for (i = 0; i < NDIST; i++) lens[i] = 5;
			build_dec(&dist, lens, NDIST);
		}
		else if (type == 2) {
			int nlen = getbits(&s, 5) + 257;
			int ndist = getbits(&s, 5) + 1;
			int ncode = getbits(&s, 4) + 4;
			struct huffdec cl;

			memset(lens, 0, sizeof(lens));
			for (i = 0; i < ncode; i++)
				lens[clen_order[i]] = getbits(&s, 3);
			build_dec(&cl, lens, 19);
			for (i = 0; i < nlen + ndist; ) {
				int sym = decode(&s, &cl), rep, val = 0;

				if (sym < 16) {
					lens[i++] = sym;
					continue;
				}
				if (sym == 16) {
					if (!i)
						die("repeat with no previous length");
					val = lens[i - 1];
					rep = 3 + getbits(&s, 2);
				}
				else if (sym == 17)
					rep = 3 + getbits(&s, 3);
				else
					rep = 11 + getbits(&s, 7);
				while (rep--)
					lens[i++] = val;
			}
			build_dec(&lit, lens, nlen);
			build_dec(&dist, lens + nlen, ndist);
		}
		else
			die("invalid block type");

		while (1) {
			int sym = decode(&s, &lit);

			lc[sym]++;
			if (sym == 256)
				break;
			if (sym > 256) {
				sym -= 257;
				getbits(&s, len_extra[sym]);
				sym = decode(&s, &dist);
				dc[sym]++;
				getbits(&s, dist_extra[sym]);
			}
		}
	} while (!last);
}

/***************************************************************************
 * package-merge
 ***************************************************************************/

/* the tail reserves up to NRSV 11-bit slots as extra symbols, see -t */
#define NRSV 128

struct item {
	unsigned long long w;
	unsigned char cnt[NLIT + NRSV];
};

static int cmp_item(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	return (x->w > y->w) - (x->w < y->w);
}

/* fills <lens> with lengths limited to <max> for the <n> weights in <w>. All
 * symbols of non-zero weight get a code.
 */
static void package_merge(const unsigned long long *w, int n, int max, int *lens)
{
	struct item *leaves, *prev, *cur;
	int nleaves = 0, nprev, ncur, i, j, k;

	leaves = calloc(n, sizeof(*leaves));
	prev = calloc(2 * n, sizeof(*prev));
	cur = calloc(2 * n, sizeof(*cur));
	for (i = 0; i < n; i++) {
		lens[i] = 0;
		if (w[i]) {
			leaves[nleaves].w = w[i];
			leaves[nleaves].cnt[i] = 1;
			nleaves++;
		}
	}
	if (nleaves < 2) {
		for (i = 0; i < n; i++)
			lens[i] = !!w[i];
		goto out;
	}
	if ((1 << max) < nleaves)
		die("too many symbols for this length limit");

	qsort(leaves, nleaves, sizeof(*leaves), cmp_item);
	memcpy(prev, leaves, nleaves * sizeof(*leaves));
	nprev = nleaves;
	for (k = 1; k < max; k++) {
		/* merge the leaves with pairs of the previous list */
		ncur = 0;
		i = 0; j = 0;
		while (i < nleaves || j + 1 < nprev) {
			unsigned long long pw = (j + 1 < nprev) ? prev[j].w + prev[j + 1].w : ~0ULL;

			if (i < nleaves && leaves[i].w <= pw)
				cur[ncur++] = leaves[i++];
			else {
				int s;

				cur[ncur].w = pw;
				for (s = 0; s < n; s++)
					cur[ncur].cnt[s] = prev[j].cnt[s] + prev[j + 1].cnt[s];
				ncur++;
				j += 2;
			}
		}
		memcpy(prev, cur, ncur * sizeof(*cur));
		nprev = ncur;
	}
	for (i = 0; i < 2 * nleaves - 2; i++)
		for (j = 0; j < n; j++)
			lens[j] += prev[i].cnt[j];
 out:
	free(leaves); free(prev); free(cur);
}

/***************************************************************************
 * table generation
 ***************************************************************************/

/* canonical codes for <n> lengths, not reversed */
static void make_codes(const int *lens, int n, int *codes)
{
	int bl_count[16] = { 0 }, next[16];
	int code = 0, i;

	for (i = 0; i < n; i++)
		bl_count[lens[i]]++;
	bl_count[0] = 0;
	for (i = 1; i < 16; i++) {
		code = (code + bl_count[i - 1]) << 1;
		next[i] = code;
	}
	for (i = 0; i < n; i++)
		codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

static int rev(int code, int len)
{
	int out = 0;

	while (len--) {
		out = (out << 1) | (code & 1);
		code >>= 1;
	}
	return out;
}

/* bit writer for the header, LSB first like the encoder */
static unsigned char hdr[256];
static int hdr_bits;

static void putbits(int val, int n)
{
	while (n--) {
		if (val & 1)
			hdr[hdr_bits >> 3] |= 1 << (hdr_bits & 7);
		val >>= 1;
		hdr_bits++;
	}
}

/* emits into hdr[] the dynamic header for these lengths, without the 3 bits
 * of BFINAL+BTYPE.
 */
static void make_header(const int *llens, const int *dlens)
{
	int all[NLIT + NDIST], sym[NLIT + NDIST], ext[NLIT + NDIST];
	unsigned long long freq[19] = { 0 };
	int clens[19], ccodes[19];
	int nlen, ndist, ncode, nsym = 0, i, n;

	for (nlen = 286; nlen > 257 && !llens[nlen - 1]; nlen--)
		;
	for (ndist = NDIST; ndist > 1 && !dlens[ndist - 1]; ndist--)
		;
	memcpy(all, llens, nlen * sizeof(int));
	memcpy(all + nlen, dlens, ndist * sizeof(int));
	n = nlen + ndist;

	/* run-length encode the lengths */
	for (i = 0; i < n; ) {
		int run = 1;

		while (i + run < n && all[i + run] == all[i])
			run++;
		if (!all[i] && run >= 11) {
			run = run > 138 ? 138 : run;
			sym[nsym] = 18; ext[nsym++] = run - 11;
		}
		else if (!all[i] && run >= 3) {
			sym[nsym] = 17; ext[nsym++] = run - 3;
		}
		else if (all[i] && run >= 4) {
			/* the value once, then as many repeats of 3 to 6 as
			 * possible, and the remainder as plain values.
			 */
			int rem = run - 1;

			sym[nsym] = all[i]; ext[nsym++] = 0;
			while (rem >= 3) {
				int r = rem > 6 ? 6 : rem;

				/* don't leave 1 or 2 behind if a split avoids it */
				if (rem > 6 && rem - 6 < 3)
					r = rem - 3;
				sym[nsym] = 16; ext[nsym++] = r - 3;
				rem -= r;
			}
			while (rem--) {
				sym[nsym] = all[i]; ext[nsym++] = 0;
			}
		}
		else {
			run = 1;
			sym[nsym] = all[i]; ext[nsym++] = 0;
		}
		i += run;
	}

	for (i = 0; i < nsym; i++)
		freq[sym[i]]++;
	package_merge(freq, 19, 7, clens);
	make_codes(clens, 19, ccodes);
	for (ncode = 19; ncode > 4 && !clens[clen_order[ncode - 1]]; ncode--)
		;

	memset(hdr, 0, sizeof(hdr));
	hdr_bits = 0;
	putbits(nlen - 257, 5);
	putbits(ndist - 1, 5);
	putbits(ncode - 4, 4);
	for (i = 0; i < ncode; i++)
		putbits(clens[clen_order[i]], 3);
	for (i = 0; i < nsym; i++) {
		putbits(rev(ccodes[sym[i]], clens[sym[i]]), clens[sym[i]]);
		if (sym[i] == 16)
			putbits(ext[i], 2);
		else if (sym[i] == 17)
			putbits(ext[i], 3);
		else if (sym[i] == 18)
			putbits(ext[i], 7);
	}
}

static void read_table(FILE *f, unsigned long long *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (fscanf(f, " %llu ,", &v[i]) != 1)
			die("short table");
}

static void read_lengths(const char *name, int *llens, int *dlens)
{
	unsigned long long v[NLIT + NDIST];
	FILE *f = fopen(name, "r");
	int i;

	if (!f)
		die("cannot open lengths");
	read_table(f, v, NLIT);
	read_table(f, v + NLIT, NDIST);
	fclose(f);
	for (i = 0; i < NLIT; i++)
		llens[i] = v[i];
	for (i = 0; i < NDIST; i++)
		dlens[i] = v[NLIT + i];
}

/* The length and distance tables hold each code with its extra bits in 16
 * bits (see text_len[] and text_dist_add[]), so these codes must not exceed
 * 11 bits. Only literals may be longer.
 */
static void check_formats(const int *llens, const int *dlens)
{
	int i;

	for (i = 257; i < NLIT; i++)
		if (llens[i] > 11)
			die("length codes must not exceed 11 bits");
	for (i = 0; i < NDIST; i++)
		if (dlens[i] > 11)
			die("distance codes must not exceed 11 bits");
}

static void print_table(const unsigned long long *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		printf("%llu%s", v[i], i == n - 1 ? "\n" : (i & 15) == 15 ? ",\n" : ",");
}

static void generate(const char *name)
{
	int llens[NLIT], dlens[NDIST], lcodes[NLIT], dcodes[NDIST];
	int i, maxlen;

	read_lengths(name, llens, dlens);
	check_formats(llens, dlens);

	make_codes(llens, NLIT, lcodes);
	make_codes(dlens, NDIST, dcodes);
	make_header(llens, dlens);

	printf("/* Huffman tree for text contents, generated by tools/mktext.c from\n"
	       " * tools/text-tree.txt. The dynamic block header that declares it is\n"
	       " * text_hdr[], made of TEXT_HDR_BITS bits, excluding BFINAL and BTYPE.\n"
	       " */\n");
	printf("#define TEXT_HDR_BITS %d\n", hdr_bits);
	printf("#define TEXT_EOB_BITS %d\n", llens[256]);
	printf("static const unsigned char text_hdr[%d] = {", (hdr_bits + 7) / 8);
	for (i = 0; i < (hdr_bits + 7) / 8; i++)
		printf("%s0x%02x,", (i % 12) ? " " : "\n\t", hdr[i]);
	printf("\n};\n\n");

	/* codes up to 12 bits fit in 16 bits with their length */
	for (i = maxlen = 0; i < NLIT; i++)
		if (llens[i] > maxlen)
			maxlen = llens[i];
	if (maxlen <= 12) {
		printf("/* same format as fixed_huff[] */\n");
		printf("static const uint16_t text_huff[%d] = {", NLIT);
	} else {
		printf("/* same format as fixed_huff[], but 32-bit since the rare literals have\n"
		       " * up to %d-bit codes.\n"
		       " */\n", maxlen);
		printf("static const uint32_t text_huff[%d] = {", NLIT);
	}
	for (i = 0; i < NLIT; i++)
		printf("%s0x%0*x,%s", (i % 8) ? " " : "\n\t", maxlen <= 12 ? 4 : 5,
		       rev(lcodes[i], llens[i]) * 16 + llens[i],
		       (i % 8) == 7 ? "" : "");
	printf("\n};\n\n");

	printf("/* same format as len_fh[] */\n");
	printf("static const uint32_t text_len[259] = {\n\t0x000000, 0x000000, 0x000000,");
	for (i = 3; i < 259; i++) {
		int c = 0, e, bits, code;

		while (c < 28 && len_base[c + 1] <= i)
			c++;
		e = i - len_base[c];
		bits = llens[257 + c] + len_extra[c];
		code = rev(lcodes[257 + c], llens[257 + c]) | (e << llens[257 + c]);
		if (!llens[257 + c])
			bits = code = 0;
		printf("%s0x%06x,", (i % 6) ? " " : "\n\t", (bits << 16) | code);
	}
	printf("\n};\n\n");

	/* The distance tables are indexed by the 5-bit reversed fixed code
	 * found in fh_dist_table[], see text_dist() in slz.c.
	 */
	printf("/* distance codes indexed by the reversed 5-bit fixed code, used to build\n"
	       " * text_dist_table[] at boot when it's not precomputed, see text_dist().\n"
	       " */\n");
	printf("static const uint32_t text_dist_add[32] = {");
	for (i = 0; i < 32; i++) {
		int c = rev(i, 5);
		uint32_t v = 0;

		if (c < NDIST)
			v = (rev(dcodes[c], dlens[c]) << 5) + dlens[c] - 5;
		printf("%s0x%08x,", (i % 6) ? " " : "\n\t", v);
	}
	printf("\n};\n\n");
	printf("static const uint8_t text_dist_shift[32] = {");
	for (i = 0; i < 32; i++) {
		int c = rev(i, 5);
		printf("%s%d,", (i % 16) ? " " : "\n\t", c < NDIST ? dlens[c] + 5 : 0);
	}
	printf("\n};\n");
}

static void generate_dist(const char *name)
{
	int llens[NLIT], dlens[NDIST], dcodes[NDIST];
	int d, c = 0;

	read_lengths(name, llens, dlens);
	check_formats(llens, dlens);
	make_codes(dlens, NDIST, dcodes);

	printf("/* Same as fh_dist_table[] for the text tree, generated by tools/mktext.c\n"
	       " * from tools/text-tree.txt.\n"
	       " */\n");
	printf("static const uint32_t text_dist_table[32768] = {\n");
	for (d = 0; d < 32768; d++) {
		uint32_t v;

		while (c < NDIST - 1 && dist_base[c + 1] <= d + 1)
			c++;
		v = (rev(dcodes[c], dlens[c]) | ((d + 1 - dist_base[c]) << dlens[c])) << 5;
		v += dist_extra[c] + dlens[c];
		printf("%s0x%08x,%s", (d % 8) ? " " : "\t", v,
		       (d % 8) == 7 ? "" : "");
		if (d % 8 == 7)
			printf(" // %d-%d\n", d - 7, d);
	}
	printf("};\n");
}

/* parses a list of symbol ranges such as "0-8,11-31,126-255" into <set> */
static void parse_set(const char *str, char *set, int n)
{
	while (*str) {
		char *end;
		long a = strtol(str, &end, 0), b = a;

		if (end == str)
			die("bad symbol list");
		if (*end == '-')
			b = strtol(end + 1, &end, 0);
		if (a < 0 || b >= n || a > b)
			die("bad symbol range");
		while (a <= b)
			set[a++] = 1;
		str = end + (*end == ',');
	}
}

/* Fills <lens> for the <w> weights so that the symbols in <tail> all get
 * <tlen> bits (or tlen-1 for a few of them so that the tree is complete), and
 * the other ones are limited to <max> bits. The tail takes the place of
 * extra symbols that are lighter than any other one, hence of <max> bits,
 * each standing for 2^(tlen-max) tail symbols.
 */
static void tail_lengths(const unsigned long long *w, const char *tail, int tlen, int max, int *lens)
{
	unsigned long long w2[NLIT + NRSV];
	int l2[NLIT + NRSV];
	int per, nt = 0, slots, extra, i, k;

	if (tlen <= max || tlen > 15)
		die("tail length must be above max and at most 15");
	per = 1 << (tlen - max);
	for (i = 0; i < NLIT; i++)
		nt += tail[i];
	slots = (nt + per - 1) / per;
	if (slots > NRSV)
		die("tail too large for this length");
	for (i = 0; i < NLIT; i++)
		w2[i] = tail[i] ? 0 : w[i] * 2;
	for (i = 0; i < slots; i++)
		w2[NLIT + i] = 1;
	package_merge(w2, NLIT + slots, max, l2);
	for (i = 0; i < slots; i++)
		if (l2[NLIT + i] != max)
			die("tail slots did not get the longest codes");
	/* each tail symbol moved up one level frees one leaf at <tlen> */
	extra = slots * per - nt;
	for (i = k = 0; i < NLIT; i++) {
		lens[i] = l2[i];
		if (tail[i])
			lens[i] = (k++ < extra) ? tlen - 1 : tlen;
	}
}

int main(int argc, char **argv)
{
	unsigned long long lc[NLIT] = { 0 }, dc[NDIST] = { 0 };
	int llens[NLIT], dlens[NDIST];
	int i, max = 9, tlen = 0, navg = 0;
	char tail[NLIT] = { 0 };
	const char *avg[16];

	if (argc < 3)
		die("usage: mktext -c raw... | -t [-m max] [-l len syms] [-a a-b]... counts | -g|-d lengths | -e lengths counts");

	if (strcmp(argv[1], "-c") == 0) {
		for (i = 2; i < argc; i++) {
			FILE *f = fopen(argv[i], "r");
			unsigned char *buf;
			long len;

			if (!f)
				die("cannot open stream");
			fseek(f, 0, SEEK_END);
			len = ftell(f);
			rewind(f);
			buf = malloc(len);
			if (fread(buf, 1, len, f) != (size_t)len)
				die("read error");
			fclose(f);
			count_stream(buf, len, lc, dc);
			free(buf);
		}
		print_table(lc, NLIT);
		print_table(dc, NDIST);
	}
	else if (strcmp(argv[1], "-t") == 0) {
		FILE *f;

		while (argc > 4 && argv[2][0] == '-') {
			if (strcmp(argv[2], "-m") == 0)
				max = atoi(argv[3]);
			else if (strcmp(argv[2], "-l") == 0 && argc > 5) {
				tlen = atoi(argv[3]);
				parse_set(argv[4], tail, 256);
				argv++; argc--;
			}
			else if (strcmp(argv[2], "-a") == 0) {
				if (navg == 16)
					die("too many -a");
				avg[navg++] = argv[3];
			}
			else
				die("unknown option for -t");
			argv += 2; argc -= 2;
		}
		f = fopen(argv[2], "r");
		if (!f)
			die("cannot open counts");
		read_table(f, lc, NLIT);
		read_table(f, dc, NDIST);
		fclose(f);
		/* -a a-b: symbols a to b (0-287 literals/lengths, 288-317
		 * distances) get the same count, hence the same length, which
		 * makes a run that is cheap in the block header.
		 */
		for (i = 0; i < navg; i++) {
			unsigned long long sum = 0, *v;
			int a, b, j;

			if (sscanf(avg[i], "%d-%d", &a, &b) != 2 || a > b || b >= NLIT + NDIST ||
			    (a < NLIT && b >= NLIT))
				die("bad range for -a");
			v = (a < NLIT) ? lc : dc - NLIT;
			for (j = a; j <= b; j++)
				sum += v[j];
			for (j = a; j <= b; j++)
				v[j] = sum / (b - a + 1);
		}
		/* the encoder may emit every literal, the EOB and every length
		 * from 4 to 258 (codes 258 to 285), and every distance, so all
		 * of them need a code. Length 3 (code 257) is never emitted.
		 */
		for (i = 0; i < 286; i++)
			if (i != 257)
				lc[i] = lc[i] * 2 + 1;
		lc[257] = 0;
		lc[286] = lc[287] = 0;
		for (i = 0; i < NDIST; i++)
			dc[i] = dc[i] * 2 + 1;
		if (tlen)
			tail_lengths(lc, tail, tlen, max, llens);
		else
			package_merge(lc, NLIT, max, llens);
		package_merge(dc, NDIST, max, dlens);
		for (i = 0; i < NLIT; i++)
			lc[i] = llens[i];
		for (i = 0; i < NDIST; i++)
			dc[i] = dlens[i];
		print_table(lc, NLIT);
		print_table(dc, NDIST);
	}
	else if (strcmp(argv[1], "-g") == 0)
		generate(argv[2]);
	else if (strcmp(argv[1], "-d") == 0)
		generate_dist(argv[2]);
	else if (strcmp(argv[1], "-e") == 0 && argc > 3) {
		unsigned long long tbits = 0, fbits = 0;
		FILE *f;

		read_lengths(argv[2], llens, dlens);
		make_header(llens, dlens);
		f = fopen(argv[3], "r");
		if (!f)
			die("cannot open counts");
		read_table(f, lc, NLIT);
		read_table(f, dc, NDIST);
		fclose(f);
		for (i = 0; i < NLIT; i++) {
			tbits += lc[i] * llens[i];
			fbits += lc[i] * (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
		}
		for (i = 0; i < NDIST; i++) {
			tbits += dc[i] * dlens[i];
			fbits += dc[i] * 5;
		}
		printf("fixed: %llu bytes, text: %llu bytes (%+.2f%%), header: %d bits\n",
		       fbits / 8, tbits / 8, (tbits * 100.0 / fbits) - 100.0, hdr_bits);
	}
	else
		die("unknown mode");
	return 0;
}
