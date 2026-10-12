PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
DEPS   := wayland-client xkbcommon
LIBS   := $(shell pkg-config --cflags --libs $(DEPS)) -lm

# Local protocol XMLs (wlroots protocols aren't in wayland-protocols);
# xdg-shell is needed because layer-shell references xdg_popup.
XDG_SHELL := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
PROTOS    := wlr-layer-shell-unstable-v1 wlr-virtual-pointer-unstable-v1 xdg-shell
HEADERS   := $(PROTOS:%=%-client-protocol.h)
SOURCES   := $(PROTOS:%=%-protocol.c)

vim-mouse: vim-mouse.c $(HEADERS) $(SOURCES)
	$(CC) $(CFLAGS) -I. -o $@ vim-mouse.c $(SOURCES) $(LIBS)

xdg-shell.xml:
	cp $(XDG_SHELL) $@

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

install: vim-mouse
	install -Dm755 vim-mouse $(PREFIX)/bin/vim-mouse

clean:
	rm -f vim-mouse xdg-shell.xml $(HEADERS) $(SOURCES)

.PHONY: install clean
