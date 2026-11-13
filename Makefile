CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# Prefer the wide-char ncurses so unicode bars render; fall back to plain ncurses.
NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null || pkg-config --cflags ncurses 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncursesw 2>/dev/null || pkg-config --libs ncurses 2>/dev/null || echo -lncurses) -ldl

all: taskmgr

taskmgr: taskmgr.c
	$(CC) $(CFLAGS) -DTASKMGR_VERSION='"$(VERSION)"' $(NCURSES_CFLAGS) -o $@ $< $(NCURSES_LIBS)

# Even smaller binary (strips symbols, drops unused sections).
small: CFLAGS = -Os -Wall -std=c11 -ffunction-sections -fdata-sections
small: taskmgr
	strip $< 2>/dev/null || true

# Release build used by CI: optimised, stripped, dynamic ncurses (so NVML dlopen keeps working).
release: CFLAGS = -O2 -Wall -Wextra -std=c11 -ffunction-sections -fdata-sections -Wl,--gc-sections
release: taskmgr
	strip taskmgr

install: taskmgr
	install -Dm755 taskmgr $(DESTDIR)$(PREFIX)/bin/taskmgr

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/taskmgr

clean:
	rm -f taskmgr

.PHONY: all small release install uninstall clean
