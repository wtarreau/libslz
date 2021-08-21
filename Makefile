TOPDIR     := $(PWD)
DESTDIR    :=
PREFIX     := /usr/local
LIBDIR     := $(PREFIX)/lib

CROSS_COMPILE :=
PLATFORM := $(shell $(CC) -dumpmachine | sed -e 's,-gnu[^-]*,,; s,.*-,,')

CC         := $(CROSS_COMPILE)gcc
OPT_CFLAGS := -O3
CPU_CFLAGS := -fomit-frame-pointer
DEB_CFLAGS := -Wall -g
DEF_CFLAGS :=
USR_CFLAGS :=
LIB_CFLAGS := -fPIC -DPRECOMPUTE_TABLES
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
ifneq ($(filter darwin%, $(PLATFORM)),)
	SHARED := libslz.dylib
	SONAME := libslz.1.dylib
else
	SHARED := libslz.so
	SONAME := $(SHARED).1
endif
OBJS       :=
OBJS       += $(patsubst %.c,%.o,$(wildcard src/*.c))
OBJS       += $(patsubst %.S,%.o,$(wildcard src/*.S))

all: static shared tools

static: $(STATIC)

shared: $(SHARED)

tools: $(BINS)

zdec: src/zdec.o
	$(LD) $(LDFLAGS) -o $@ $^

zenc: src/zenc.o src/slz.o
	$(LD) $(LDFLAGS) -o $@ $^

$(STATIC): src/slz.o
	$(AR) rv $@ $^

$(SONAME): src/slz-pic.o
ifneq ($(filter darwin%, $(PLATFORM)),)
	$(LD) -dynamiclib $(LDFLAGS) -install_name $@ -o $@ $^
else
	$(LD) -shared $(LDFLAGS) -Wl,-soname,$@ -o $@ $^
endif

$(SHARED): $(SONAME)
	ln -sf $^ $@

%.o: %.c src/slz.h src/tables.h
	$(CC) $(CFLAGS) -o $@ -c $<

%-pic.o: %.c src/slz.h src/tables.h
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -o $@ -c $<

install: install-headers install-static install-shared install-tools

install-headers:
	[ -d "$(DESTDIR)$(PREFIX)/include/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/include
	cp src/slz.h $(DESTDIR)$(PREFIX)/include/slz.h
	chmod 644 $(DESTDIR)$(PREFIX)/include/slz.h

install-static: static
	[ -d "$(DESTDIR)$(LIBDIR)/." ] || mkdir -p -m 0755 $(DESTDIR)$(LIBDIR)
	cp $(STATIC) $(DESTDIR)$(LIBDIR)/$(STATIC)
	chmod 644 $(DESTDIR)$(LIBDIR)/$(STATIC)

install-shared: shared
	[ -d "$(DESTDIR)$(LIBDIR)/." ] || mkdir -p -m 0755 $(DESTDIR)$(LIBDIR)
	cp    $(SONAME) $(DESTDIR)$(LIBDIR)/$(SONAME)
	cp -P $(SHARED) $(DESTDIR)$(LIBDIR)/$(SHARED)
	chmod 644 $(DESTDIR)$(LIBDIR)/$(SONAME)

install-tools: tools
	$(STRIP) zenc
	[ -d "$(DESTDIR)$(PREFIX)/bin/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/bin
	cp zdec $(DESTDIR)$(PREFIX)/bin/zdec
	cp zenc $(DESTDIR)$(PREFIX)/bin/zenc
	chmod 755 $(DESTDIR)$(PREFIX)/bin/zdec
	chmod 755 $(DESTDIR)$(PREFIX)/bin/zenc

clean:
	-rm -f $(BINS) $(OBJS) $(STATIC) $(SHARED) *.[oa] *.so *.so.* *.dylib *~ */*.[oa] */*~
