TOPDIR     := $(PWD)
DESTDIR    :=
PREFIX     := /usr/local
LIBDIR     := lib

CROSS_COMPILE :=

CC         := $(CROSS_COMPILE)gcc
OPT_CFLAGS := -O3
CPU_CFLAGS := -fomit-frame-pointer -DCONFIG_REGPARM=3
DEB_CFLAGS := -Wall -g
DEF_CFLAGS :=
USR_CFLAGS :=
INC_CFLAGS := -I$(TOPDIR)/include
CFLAGS     := $(OPT_CFLAGS) $(CPU_CFLAGS) $(DEB_CFLAGS) $(DEF_CFLAGS) $(USR_CFLAGS) $(INC_CFLAGS)

LD         := $(CC)
DEB_LFLAGS := -g
USR_LFLAGS :=
LIB_LFLAGS :=
LDFLAGS    := $(DEB_LFLAGS) $(USR_LFLAGS) $(LIB_LFLAGS)

AR         := $(CROSS_COMPILE)ar
STRIP      := $(CROSS_COMPILE)strip
BINS       := zdec zenc
STATIC     := libslz.a
OBJS       :=
OBJS       += $(patsubst %.c,%.o,$(wildcard src/*.c))
OBJS       += $(patsubst %.S,%.o,$(wildcard src/*.S))

all: static tools

static: $(STATIC)

tools: $(BINS)

zdec: src/zdec.o
	$(LD) $(LDFLAGS) -o $@ $^

zenc: src/zenc.o src/slz.o
	$(LD) $(LDFLAGS) -o $@ $^

libslz.a: src/slz.o
	$(AR) rv $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $^

install: install-headers install-static install-tools

install-headers:
	[ -d "$(DESTDIR)$(PREFIX)/include/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/include
	cp src/slz.h $(DESTDIR)$(PREFIX)/include/slz.h
	chmod 644 $(DESTDIR)$(PREFIX)/include/slz.h

install-static: static
	[ -d "$(DESTDIR)$(PREFIX)/$(LIBDIR)/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	cp libslz.a $(DESTDIR)$(PREFIX)/$(LIBDIR)/libslz.a
	chmod 644 $(DESTDIR)$(PREFIX)/$(LIBDIR)/libslz.a

install-tools: tools
	$(STRIP) zenc
	[ -d "$(DESTDIR)$(PREFIX)/bin/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/bin
	cp zenc $(DESTDIR)$(PREFIX)/bin/zenc
	chmod 755 $(DESTDIR)$(PREFIX)/bin/zenc

clean:
	-rm -f $(BINS) $(OBJS) $(STATIC) *.[oa] *~ */*.[oa] */*~
