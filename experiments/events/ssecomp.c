/* ssecomp: compresses a capture of server-sent events one event at a time,
 * flushing after each one as haproxy has to when events are separated by
 * delays, with a history of up to <hist> bytes of the previous events that
 * the encoder may reference (slz_rfc1951_encode_hist(), experimental).
 * The capture has one event per line; each event is sent as the line
 * followed by "\n\n" (the SSE event terminator). The output is checked with
 * zlib.
 *
 * usage: ssecomp <file> <level> <hint> <hist> [sync|partial|eob|none]
 *   hist = -1: the whole capture in a single call (no flush), as reference
 *   sync (default): slz_rfc1951_flush() after each event
 *   partial: slz_rfc1951_pflush() after each event (experimental)
 *   eob: slz_rfc1951_eflush() after each event, EOB only (experimental)
 *   none: no flush between events (shows the cost of flushing)
 * Env: LF=1 one event per line as is (logs), EVENT=1 slz_rfc1951_encode_event()
 * (its own history, <hist> ignored), FIXED=1 open every block in the fixed
 * tree, PEREVENT=1 per-event sizes, DUMP=<file> writes the output.
 * With a flush, each event is checked to be entirely decodable from the
 * bytes emitted up to its flush.
 * prints: level hint hist events in out ratio out/event flush_bytes
 *
 * Needs the experimental functions from patches/ (see README), it doesn't
 * build against the integrated SLZ_CT_EVENT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "slz.h"

int main(int argc, char **argv)
{
	FILE *f;
	unsigned char *raw, *in, *out, *chk;
	long rlen, ilen = 0, olen = 0, flushed = 0, events = 0, hist, i, start;
	int level, hint, mode;
	struct slz_ev_ctx *ev = NULL;
	int lf = !!getenv("LF");
	struct slz_stream strm;
	z_stream d, live;
	unsigned char *lbuf;

	if (argc < 5)
		return fprintf(stderr, "usage: %s file level hint hist [noflush]\n", argv[0]), 1;
	level = atoi(argv[2]);
	hint = atoi(argv[3]);
	hist = atol(argv[4]);
	mode = argc <= 5 || !strcmp(argv[5], "sync") ? 0 : !strcmp(argv[5], "partial") ? 1 : !strcmp(argv[5], "eob") ? 3 : 2;

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

	/* build the stream of events: each line + "\n\n" */
	in = malloc(rlen * 2 + 2);
	for (i = 0; i < rlen; i++) {
		in[ilen++] = raw[i];
		if (raw[i] == '\n' && !lf)
			in[ilen++] = '\n';
	}
	out = malloc(ilen * 2 + 65536);
	lbuf = malloc(ilen + 1);
	memset(&live, 0, sizeof(live));
	inflateInit2(&live, -15);
	live.next_out = lbuf;
	live.avail_out = ilen + 1;

	slz_rfc1951_init(&strm, level);
	/* EVENT=1: slz_rfc1951_encode_event() (its own history, <hist> ignored) */
	if (getenv("EVENT")) {
		ev = aligned_alloc(64, sizeof(*ev));
		slz_ev_init(ev);
	}
	if (hint)
		slz_set_content_type(&strm, SLZ_CT_TEXT);

	if (hist < 0) {
		olen = slz_rfc1951_encode(&strm, out, in, ilen, 1);
		events = 1;
	} else {
		for (start = 0; start < ilen; ) {
			long end = start, h;

			if (lf) {
				/* LF=1: one event per line, as is */
				while (end < ilen && in[end] != '\n')
					end++;
				end = end + 1 > ilen ? ilen : end + 1;
			} else {
				while (end < ilen && !(in[end] == '\n' && end + 1 < ilen && in[end + 1] == '\n'))
					end++;
				end = end + 2 > ilen ? ilen : end + 2; /* include "\n\n" */
			}
			h = hist < start ? hist : start;
			/* FIXED=1: start every block in the fixed tree (levels 2-4) */
			if (getenv("FIXED") && strm.state == SLZ_ST_EOB)
				strm.cur_tree = SLZ_CT_GENERIC;
			if (ev)
				olen += slz_rfc1951_encode_event(&strm, out + olen, in + start, end - start, 1, ev);
			else
				olen += slz_rfc1951_encode_hist(&strm, out + olen, in + start, end - start, 1, h);
			if (mode != 2) {
				long fl = mode == 3 ? slz_rfc1951_eflush(&strm, out + olen)
				        : mode ? slz_rfc1951_pflush(&strm, out + olen)
				               : slz_rfc1951_flush(&strm, out + olen);
				long fed = (unsigned char *)live.next_in ? (unsigned char *)live.next_in - out : 0;

				olen += fl;
				flushed += fl;
				/* everything up to <end> must be decodable now */
				live.next_in = out + fed;
				live.avail_in = olen - fed;
				inflate(&live, Z_SYNC_FLUSH);
				if (live.total_out != (unsigned long)end || memcmp(lbuf + start, in + start, end - start)) {
					fprintf(stderr, "EVENT %ld NOT DELIVERED: %lu/%ld bytes decodable\n", events, live.total_out, end);
					return 3;
				}
			}
			if (getenv("PEREVENT"))
				fprintf(stderr, "ev %ld len %ld out %ld\n", events, end - start, olen);
			events++;
			start = end;
		}
	}
	olen += slz_rfc1951_finish(&strm, out + olen);

	/* check */
	chk = malloc(ilen + 1);
	memset(&d, 0, sizeof(d));
	inflateInit2(&d, -15);
	d.next_in = out; d.avail_in = olen;
	d.next_out = chk; d.avail_out = ilen + 1;
	if (inflate(&d, Z_FINISH) != Z_STREAM_END || d.total_out != (unsigned long)ilen || memcmp(chk, in, ilen)) {
		fprintf(stderr, "CHECK FAILED level=%d hint=%d hist=%ld\n", level, hint, hist);
		return 2;
	}
	inflateEnd(&d);

	if (getenv("DUMP")) {
		FILE *o = fopen(getenv("DUMP"), "wb");
		fwrite(out, 1, olen, o);
		fclose(o);
	}
	printf("%d %d %ld %ld %ld %ld %.2f%% %.2f %ld\n", level, hint, hist, events, ilen, olen,
	       100.0 * olen / ilen, (double)olen / events, flushed);
	return 0;
}
