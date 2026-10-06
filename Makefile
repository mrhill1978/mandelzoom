# mandelzoom
#
#   make                      build, tuned for this CPU
#   make install              install to /usr/local/bin (may need sudo)
#   make install PREFIX=$HOME/.local
#   make OPENMP=              build without OpenMP (e.g. Apple clang)
#
# Packagers: CFLAGS, LDFLAGS, PREFIX and DESTDIR are all honored.

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O3 -march=native
OPENMP ?= -fopenmp
LDLIBS += -lm

all: mandelzoom

mandelzoom: mandelzoom.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPENMP) $(LDFLAGS) -o $@ mandelzoom.c $(LDLIBS)

install: mandelzoom
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 mandelzoom $(DESTDIR)$(BINDIR)/mandelzoom

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mandelzoom

clean:
	rm -f mandelzoom

.PHONY: all install uninstall clean
