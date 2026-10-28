CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
LDLIBS = -lm
PREFIX ?= $(HOME)/.local

chuhfetch: chuhfetch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: chuhfetch
	install -Dm755 chuhfetch $(PREFIX)/bin/chuhfetch
	sudo setcap cap_perfmon+ep $(PREFIX)/bin/chuhfetch

clean:
	rm -f chuhfetch

.PHONY: install clean
