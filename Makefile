PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man
CFLAGS += -Wall -O2

FUSE_CFLAGS = $(shell pkg-config --cflags fuse)
FUSE_LIBS = $(shell pkg-config --libs fuse)

PCRE_CFLAGS = $(shell pkg-config --cflags libpcre)
PCRE_LIBS = $(shell pkg-config --libs libpcre)

all: rewritefs

rewritefs: rewritefs.o rewrite.o util.o
	gcc $^ $(FUSE_LIBS) $(PCRE_LIBS) $(LDFLAGS) -o $@

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
