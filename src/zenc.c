/*
 * Copyright (C) 2013-2015 Willy Tarreau <w@1wt.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <fcntl.h>
#include "slz.h"

/* some platforms do not provide PAGE_SIZE */
#ifndef PAGE_SIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#endif

/* block size for experimentations */
#define BLK 32768

/* display the message and exit with the code */
__attribute__((noreturn)) void die(int code, const char *format, ...)
{
        va_list args;

        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        exit(code);
}

__attribute__((noreturn)) void usage(const char *name, int code)
{
	die(code,
	    "Usage: %s [option]* [file]\n"
	    "\n"
	    "The following arguments are supported :\n"
	    "  -0         disable compression, only uses format\n"
	    "  -1         enable compression [default]\n"
	    "  -b <size>  only use <size> bytes from the input file\n"
	    "  -c         send output to stdout [default]\n"
	    "  -f         force sending output to a terminal\n"
	    "  -h         display this help\n"
	    "  -l <loops> loop <loops> times over the same file\n"
	    "  -t         test mode: do not emit anything\n"
	    "  -v         increase verbosity\n"
	    "\n"
	    "  -D         use raw Deflate output format (RFC1951)\n"
	    "  -G         use Gzip output format (RFC1952) [default]\n"
	    "  -Z         use Zlib output format (RFC1950)\n"
	    "\n"
	    "If no file is specified, stdin will be used instead.\n"
	    "\n"
	    ,name);
}


int main(int argc, char **argv)
{
	const char *name = argv[0];
	struct stat instat;
	struct slz_stream strm;
	unsigned char *outbuf;
	unsigned char *buffer;
	off_t toread = -1;
	off_t tocompress = 0;
	off_t ofs;
	size_t len;
	size_t mapsize = 0;
	unsigned long long totin = 0;
	unsigned long long totout = 0;
	int loops = 1;
	int console = 1;
	int level   = 1;
	int verbose = 0;
	int test    = 0;
	int format  = SLZ_FMT_GZIP;
	int force   = 0;
	int fd = 0;

	argv++;
	argc--;

	while (argc > 0) {
		if (**argv != '-')
			break;

		if (strcmp(argv[0], "-0") == 0)
			level = 0;

		else if (strcmp(argv[0], "-1") == 0)
			level = 1;

		else if (strcmp(argv[0], "-b") == 0) {
			if (argc < 2)
				usage(name, 1);
			toread = atoll(argv[1]);
			argv++;
			argc--;
		}

		else if (strcmp(argv[0], "-c") == 0)
			console = 1;

		else if (strcmp(argv[0], "-f") == 0)
			force = 1;

		else if (strcmp(argv[0], "-h") == 0)
			usage(name, 0);

		else if (strcmp(argv[0], "-l") == 0) {
			if (argc < 2)
				usage(name, 1);
			loops = atoi(argv[1]);
			argv++;
			argc--;
		}

		else if (strcmp(argv[0], "-t") == 0)
			test = 1;

		else if (strcmp(argv[0], "-v") == 0)
			verbose++;

		else if (strcmp(argv[0], "-D") == 0)
			format = SLZ_FMT_DEFLATE;

		else if (strcmp(argv[0], "-G") == 0)
			format = SLZ_FMT_GZIP;

		else if (strcmp(argv[0], "-Z") == 0)
			format = SLZ_FMT_ZLIB;

		else
			usage(name, 1);

		argv++;
		argc--;
	}

	if (argc > 0) {
		fd = open(argv[0], O_RDONLY);
		if (fd == -1) {
			perror("open()");
			exit(1);
		}
	}

	if (isatty(1) && !test && !force)
		die(1, "Use -f if you really want to send compressed data to a terminal, or -h for help.\n");

	slz_make_crc_table();
	slz_prepare_dist_table();

	outbuf = calloc(1, BLK + 4096);
	if (!outbuf) {
		perror("calloc");
		exit(1);
	}

	/* principle : we'll determine the input file size and try to map the
	 * file at once. If it works we have a single input buffer of the whole
	 * file size. If it fails we'll bound the input buffer to the buffer size
	 * and read the input in smaller blocks.
	 */
	if (toread < 0) {
		if (fstat(fd, &instat) == -1) {
			perror("fstat(fd)");
			exit(1);
		}
		toread = instat.st_size;
	}

	if (toread) {
		/* we know the size to map, let's do it */
		mapsize = (toread + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		buffer = mmap(NULL, mapsize, PROT_READ, MAP_PRIVATE, fd, 0);
		if (buffer == MAP_FAILED)
			mapsize = 0;
	}

	if (!mapsize) {
		/* no mmap() done, read the size of a default block */
		mapsize = BLK;
		if (toread && toread < mapsize)
			mapsize = toread;

		buffer = calloc(1, mapsize);
		if (!buffer) {
			perror("calloc");
			exit(1);
		}

		if (toread && toread <= mapsize) {
			/* file-at-once, read it now */
			toread = read(fd, buffer, toread);
			if (toread < 0) {
				perror("read");
				exit(2);
			}
		}

		if (loops > 1 && !toread) {
			loops = 1;
			fprintf(stderr, "Warning: disabling loops on non-regular file\n");
		}
	}

	while (loops--) {
		slz_init(&strm, level, format);

		len = ofs = 0;
		do {
			int more = !toread || (toread - ofs) > BLK;
			unsigned char *start;

			if (toread && toread <= mapsize) {
				/* We use the memory-mapped file so the next
				 * block starts at the buffer + file offset. We
				 * read by blocks of BLK bytes at one except the
				 * last one.
				 */
				tocompress = more ? BLK : toread - ofs;
				start = buffer + ofs;
			}
			else {
				tocompress = read(fd, buffer, more ? BLK : toread - ofs);
				if (tocompress <= 0) {
					if (!tocompress) // done
						break;
					perror("read");
					exit(2);
				}
				start = buffer;
			}
			len += slz_encode(&strm, outbuf + len, start, tocompress, more);

			if (more) {
				if (console && !test)
					write(1, outbuf, len);
				totout += len;
				len = 0;
			}
			ofs += tocompress;
		} while (!toread || ofs < toread);

		len += slz_finish(&strm, outbuf + len);
		totin += ofs;
		totout += len;
		if (console && !test)
			write(1, outbuf, len);

		if (loops && (!toread || toread > mapsize)) {
			/* this is a seeked file, let's rewind it now */
			lseek(fd, 0, SEEK_SET);
		}
	}
	if (verbose)
		fprintf(stderr, "totin=%llu totout=%llu ratio=%.2f%% crc32=%08x\n",
		        totin, totout, totout * 100.0 / totin, strm.crc32);

	return 0;
}
