/* compresses a buffer allocated so that the byte just past the input is
 * unaddressable, to catch reads past the end of the input buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slz.h"
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "r");
	static unsigned char big[1 << 20];
	long n = fread(big, 1, sizeof(big), f);
	long chunk = argc > 2 ? atol(argv[2]) : 32768;
	unsigned char *out = malloc(2 * chunk + 4096);
	struct slz_stream s;
	long ofs;

	memset(&s, 0, sizeof(s));
	slz_init(&s, argc > 3 ? atoi(argv[3]) : 1, SLZ_FMT_GZIP); if (argc > 4) s.content_type = SLZ_CT_TEXT;
	for (ofs = 0; ofs < n; ofs += chunk) {
		long len = (n - ofs > chunk) ? chunk : n - ofs;
		unsigned char *in = malloc(len);   /* exact size: valgrind traps len+1 */
		memcpy(in, big + ofs, len);
		slz_encode(&s, out, in, len, ofs + len < n);
		free(in);
	}
	slz_finish(&s, out);
	return 0;
}
