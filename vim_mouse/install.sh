#!/usr/bin/env bash
# ./install.sh [uninstall]   — installs into ~/.local, no root.
#   Wayland (sway): vim-mouse + patched wl-kbptr, links the wl-kbptr config.
#   X11 (Cinnamon): vim-mouse + x11-kbptr, binds Super+g / Super+c.
# The backend follows $WAYLAND_DISPLAY / $DISPLAY; override with VIM_MOUSE_BACKEND=wayland|x11.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
prefix=$HOME/.local
src=$prefix/src/wl-kbptr
x11_dev=$prefix/src/vim-mouse-x11-dev
kbptr_version=v0.4.1
patch=$PWD/wl-kbptr-null-keymap.patch
conf=${XDG_CONFIG_HOME:-$HOME/.config}/wl-kbptr/config
sway_conf=${XDG_CONFIG_HOME:-$HOME/.config}/sway/config
cinnamon_schema=org.cinnamon.desktop.keybindings
cinnamon_dir=/org/cinnamon/desktop/keybindings/custom-keybindings

detect_backend() {
	if [[ -n ${VIM_MOUSE_BACKEND:-} ]]; then
		echo "$VIM_MOUSE_BACKEND"
	elif [[ -n ${WAYLAND_DISPLAY:-} ]]; then
		echo wayland
	elif [[ -n ${DISPLAY:-} ]]; then
		echo x11
	else
		echo "no WAYLAND_DISPLAY or DISPLAY: set VIM_MOUSE_BACKEND=wayland|x11" >&2
		exit 2
	fi
}

install_wayland() {
	make install BACKEND=wayland PREFIX="$prefix"

	[[ -d $src ]] || git clone -q --depth 1 --branch "$kbptr_version" https://github.com/moverest/wl-kbptr.git "$src"
	# Crash fix: apply unless it's already in.
	git -C "$src" apply --reverse --check "$patch" 2>/dev/null || git -C "$src" apply "$patch"
	[[ -d $src/build ]] || meson setup "$src/build" "$src" --prefix="$prefix" --buildtype=release -Dopencv=disabled
	ninja -C "$src/build" install

	mkdir -p "$(dirname "$conf")"
	# Keep a hand-written file instead of silently overwriting it.
	if [[ -e $conf && ! -L $conf ]]; then
		mv "$conf" "$conf.bak"
		echo "backed up existing file to $conf.bak"
	fi
	ln -sfn "$PWD/wl-kbptr.conf" "$conf"

	if ! grep -q 'exec vim-mouse' "$sway_conf" 2>/dev/null || ! grep -q 'exec wl-kbptr' "$sway_conf" 2>/dev/null; then
		echo "add to $sway_conf, then 'swaymsg reload':"
		echo '    bindsym $mod+g exec wl-kbptr'
		echo '    bindsym $mod+c exec vim-mouse'
	fi
}

x11_headers_ok() { # [dev root]
	printf '#include <X11/extensions/XTest.h>\n#include <X11/extensions/shape.h>\n#include <cairo-xlib.h>\n' |
		"${CC:-cc}" -fsyntax-only -x c - -I"${1:-}/usr/include/cairo" ${1:+-I"$1/usr/include"} 2>/dev/null
}

# Without root on Debian/Ubuntu/Mint: unpack the -dev packages into $x11_dev
# and point its lib/ at the runtime libraries that are already installed.
fetch_x11_dev() {
	if ! command -v apt-get >/dev/null; then
		echo "missing headers: install the X11, Xtst, Xext and cairo development packages" >&2
		exit 1
	fi
	echo "fetching X11/cairo headers into $x11_dev"
	rm -rf "$x11_dev"
	mkdir -p "$x11_dev/debs" "$x11_dev/lib"
	(cd "$x11_dev/debs" && apt-get download -q libx11-dev x11proto-dev libxtst-dev libxi-dev libxext-dev libcairo2-dev)
	for deb in "$x11_dev"/debs/*.deb; do
		dpkg-deb -x "$deb" "$x11_dev"
	done
	for lib in libX11.so.6 libXtst.so.6 libXext.so.6 libcairo.so.2; do
		ln -sfn "$("${CC:-cc}" -print-file-name="$lib")" "$x11_dev/lib/${lib%.so.*}.so"
	done
}

cinnamon_ids() {
	local list
	list=$(gsettings get $cinnamon_schema custom-list)
	list=${list#@as }
	echo "${list//[][,\']/}"
}

cinnamon_set_ids() {
	local list=
	(($#)) && list=$(printf "'%s', " "$@")
	gsettings set $cinnamon_schema custom-list "@as [${list%, }]"
}

cinnamon_get() { # id key
	gsettings get "$cinnamon_schema.custom-keybinding:$cinnamon_dir/$1/" "$2"
}

cinnamon_set() { # id key value
	gsettings set "$cinnamon_schema.custom-keybinding:$cinnamon_dir/$1/" "$2" "$3"
}

# Adds a Cinnamon custom shortcut unless one already runs the command.
cinnamon_bind() { # name command accel
	local ids id n=0
	read -ra ids <<<"$(cinnamon_ids)"
	for id in "${ids[@]}"; do
		[[ $id == custom* && $(cinnamon_get "$id" command) == "'$2'" ]] && return
	done
	while [[ " ${ids[*]} " == *" custom$n "* ]]; do n=$((n + 1)); done
	id=custom$n
	cinnamon_set "$id" name "'$1'"
	cinnamon_set "$id" command "'$2'"
	cinnamon_set "$id" binding "['$3']"
	cinnamon_set_ids "${ids[@]}" "$id"
	echo "bound $3 to $2"
}

cinnamon_unbind() { # command...
	local ids keep=() id cmd
	read -ra ids <<<"$(cinnamon_ids)"
	for id in "${ids[@]}"; do
		if [[ $id == custom* ]]; then
			cmd=$(cinnamon_get "$id" command)
			if [[ " $* " == *" ${cmd//\'/} "* ]]; then
				dconf reset -f "$cinnamon_dir/$id/"
				continue
			fi
		fi
		keep+=("$id")
	done
	cinnamon_set_ids "${keep[@]}"
}

has_cinnamon() {
	command -v gsettings >/dev/null && gsettings list-keys $cinnamon_schema >/dev/null 2>&1
}

install_x11() {
	local dev=
	if ! x11_headers_ok; then
		x11_headers_ok "$x11_dev" || fetch_x11_dev
		dev=$x11_dev
	fi
	make install BACKEND=x11 PREFIX="$prefix" X11_DEV="$dev"

	if has_cinnamon; then
		cinnamon_bind 'x11-kbptr' "$prefix/bin/x11-kbptr" '<Super>g'
		cinnamon_bind 'vim-mouse' "$prefix/bin/vim-mouse" '<Super>c'
	else
		echo "bind in your window manager:"
		echo "    Super+g -> $prefix/bin/x11-kbptr"
		echo "    Super+c -> $prefix/bin/vim-mouse"
	fi
}

case ${1:-install} in
install)
	case $(detect_backend) in
	wayland) install_wayland ;;
	x11) install_x11 ;;
	*) echo "VIM_MOUSE_BACKEND must be wayland or x11" >&2; exit 2 ;;
	esac
	echo "installed"
	;;
uninstall)
	rm -f "$prefix/bin/vim-mouse" "$prefix/bin/wl-kbptr" "$prefix/bin/x11-kbptr" \
		"$prefix/share/applications/wl-kbptr.desktop"
	if [[ -L $conf && $(readlink -f "$conf") == "$PWD/wl-kbptr.conf" ]]; then
		rm "$conf"
		[[ -e $conf.bak ]] && mv "$conf.bak" "$conf"
	fi
	if has_cinnamon; then
		cinnamon_unbind "$prefix/bin/x11-kbptr" "$prefix/bin/vim-mouse"
	fi
	echo "uninstalled (sources left in $src and $x11_dev; remove any \$mod+g / \$mod+c bindings from $sway_conf)"
	;;
*)
	echo "usage: $0 [install|uninstall]" >&2
	exit 2
	;;
esac
