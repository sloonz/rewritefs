PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man
CFLAGS = -Wall -O2 -DHAVE_FDATASYNC=1 -DHAVE_SETXATTR=1
LDFLAGS = 

FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LIBS = $(shell pkg-config --libs fuse3) -lulockmgr

PCRE_CFLAGS = $(shell pkg-config --cflags libpcre)
PCRE_LIBS = $(shell pkg-config --libs libpcre)

all: rewritefs

rewritefs: rewritefs.o rewrite.o
	gcc rewritefs.o rewrite.o $(FUSE_LIBS) $(PCRE_LIBS) $(LDFLAGS) -o $@

%.o: %.c
	gcc $(CFLAGS) $(FUSE_CFLAGS) $(PCRE_CFLAGS) -c $< -o $@

clean:
	rm -f rewritefs *.o

install: rewritefs
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)/man1
	install --mode=6755 rewritefs $(DESTDIR)$(BINDIR)
	install --mode=644 rewritefs.1 $(DESTDIR)$(MANDIR)/man1
	ln -s rewritefs $(DESTDIR)$(BINDIR)/mount.rewritefs

test: all
	(cd tests ; bats tests.bats)
