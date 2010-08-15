PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
CFLAGS = -Wall -O2
LDFLAGS = 

FUSE_CFLAGS = $(shell pkg-config --cflags fuse)
FUSE_LIBS = $(shell pkg-config --libs fuse)

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
	install --mode=755 rewritefs $(DESTDIR)$(BINDIR)
