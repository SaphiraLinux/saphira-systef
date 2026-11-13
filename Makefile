CC       ?= gcc
CFLAGS   ?= -std=gnu11 -Os -Wall -Wextra

# Static linking is the Linux/musl-free-distro ideal; macOS and most *BSD
# toolchains reject -static against the system libc.  Override with
#   make STATIC=1     (force static anywhere)
# or set LDFLAGS explicitly.
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    STATIC_DEFAULT := 0
else ifeq ($(UNAME_S),FreeBSD)
    STATIC_DEFAULT := 1
else ifeq ($(UNAME_S),OpenBSD)
    STATIC_DEFAULT := 1
else
    STATIC_DEFAULT := 1
endif
STATIC   ?= $(STATIC_DEFAULT)
ifeq ($(STATIC),1)
    LDFLAGS ?= -static
else
    LDFLAGS ?=
endif

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
MAN1DIR  ?= $(MANDIR)/man1
DESTDIR  ?=
SYSCONFDIR ?= /etc
CONFDIR  ?= $(SYSCONFDIR)/akaman
CONF     ?= $(CONFDIR)/akamcp.conf

SRC := src/main.c src/core.c src/mcp.c src/json.c
OBJ := $(SRC:.c=.o)
BIN := akaman

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)
	strip $@

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	bash tests/test.sh

bench: all
	./$(BIN) --bench

clean:
	rm -f $(OBJ) $(BIN)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MAN1DIR)
	install -m 0644 docs/akaman.1 $(DESTDIR)$(MAN1DIR)/$(BIN).1
	install -d $(DESTDIR)$(CONFDIR)
	if test -e $(DESTDIR)$(CONF); then chmod 0600 $(DESTDIR)$(CONF); else install -m 0600 /dev/null $(DESTDIR)$(CONF); fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MAN1DIR)/$(BIN).1
	-rmdir $(DESTDIR)$(MAN1DIR) $(DESTDIR)$(MANDIR) 2>/dev/null || true

.PHONY: all test bench clean install uninstall
