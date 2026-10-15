PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin

# wayland: ext-session-lock-v1 (Sway, Hyprland, ...); x11: Xorg sessions.
BACKEND  ?= wayland

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -D_GNU_SOURCE
LDLIBS   += -lpam

ifeq ($(BACKEND),wayland)
PKGS      := wayland-client xkbcommon
PROTO_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-session-lock/ext-session-lock-v1.xml
SRCS      := src/wayland.c build/ext-session-lock-v1-protocol.c
DEPS      := build/ext-session-lock-v1-client-protocol.h
CFLAGS    += -Ibuild
else ifeq ($(BACKEND),x11)
PKGS      := x11
SRCS      := src/x11.c
else
$(error BACKEND must be wayland or x11)
endif

CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

dark-screenlock: $(SRCS) src/core.c src/core.h $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) src/core.c $(LDLIBS)

build/ext-session-lock-v1-client-protocol.h: $(PROTO_XML)
	@mkdir -p build
	wayland-scanner client-header $< $@

build/ext-session-lock-v1-protocol.c: $(PROTO_XML)
	@mkdir -p build
	wayland-scanner private-code $< $@

install: dark-screenlock
	install -Dm755 dark-screenlock $(DESTDIR)$(BINDIR)/dark-screenlock

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dark-screenlock

clean:
	rm -rf build dark-screenlock

.PHONY: install uninstall clean
