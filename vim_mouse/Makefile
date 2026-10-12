PREFIX  ?= $(HOME)/.local
# wayland (sway / wlroots) or x11 (e.g. Cinnamon)
BACKEND ?= wayland
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
OUT     := build/$(BACKEND)

.DEFAULT_GOAL := all

ifeq ($(BACKEND),wayland)

LIBS := $(shell pkg-config --cflags --libs wayland-client xkbcommon) -lm

# Local protocol XMLs (wlroots protocols aren't in wayland-protocols);
# xdg-shell is needed because layer-shell references xdg_popup.
XDG_SHELL := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
PROTOS    := wlr-layer-shell-unstable-v1 wlr-virtual-pointer-unstable-v1 xdg-shell
HEADERS   := $(PROTOS:%=%-client-protocol.h)
SOURCES   := $(PROTOS:%=%-protocol.c)
BINS      := $(OUT)/vim-mouse

$(OUT)/vim-mouse: vim-mouse.c vim-mouse.h backend-wayland.c $(HEADERS) $(SOURCES) | $(OUT)
	$(CC) $(CFLAGS) -I. -o $@ vim-mouse.c backend-wayland.c $(SOURCES) $(LIBS)

xdg-shell.xml:
	cp $(XDG_SHELL) $@

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

else ifeq ($(BACKEND),x11)

# X11_DEV: -dev packages unpacked without root by install.sh when the system
# lacks the headers; its lib/ links to the installed runtime libraries.
X11_DEV   ?=
X11_FLAGS := -I$(X11_DEV)/usr/include/cairo $(if $(X11_DEV),-I$(X11_DEV)/usr/include -L$(X11_DEV)/lib)
BINS      := $(OUT)/vim-mouse $(OUT)/x11-kbptr

$(OUT)/vim-mouse: vim-mouse.c vim-mouse.h backend-x11.c | $(OUT)
	$(CC) $(CFLAGS) $(X11_FLAGS) -o $@ vim-mouse.c backend-x11.c -lX11 -lXext -lXtst -lm

$(OUT)/x11-kbptr: x11-kbptr.c | $(OUT)
	$(CC) $(CFLAGS) $(X11_FLAGS) -o $@ x11-kbptr.c -lX11 -lXtst -lcairo -lm

else
$(error BACKEND must be wayland or x11)
endif

all: $(BINS)

$(OUT):
	mkdir -p $@

install: $(BINS)
	install -Dm755 -t $(PREFIX)/bin $(BINS)

clean:
	rm -rf build xdg-shell.xml *-client-protocol.h *-protocol.c

.PHONY: all install clean
