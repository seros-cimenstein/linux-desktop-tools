#!/usr/bin/env bash
# ./install.sh [uninstall]   — builds vim-mouse and patched wl-kbptr into ~/.local, links the wl-kbptr config.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
prefix=$HOME/.local
src=$prefix/src/wl-kbptr
kbptr_version=v0.4.1
patch=$PWD/wl-kbptr-null-keymap.patch
conf=${XDG_CONFIG_HOME:-$HOME/.config}/wl-kbptr/config
sway_conf=${XDG_CONFIG_HOME:-$HOME/.config}/sway/config

case ${1:-install} in
install)
	make install PREFIX="$prefix"

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
	echo "installed"
	;;
uninstall)
	rm -f "$prefix/bin/vim-mouse" "$prefix/bin/wl-kbptr" "$prefix/share/applications/wl-kbptr.desktop"
	if [[ -L $conf && $(readlink -f "$conf") == "$PWD/wl-kbptr.conf" ]]; then
		rm "$conf"
		[[ -e $conf.bak ]] && mv "$conf.bak" "$conf"
	fi
	echo "uninstalled (source left in $src; remove the \$mod+g / \$mod+c bindings from $sway_conf)"
	;;
*)
	echo "usage: $0 [install|uninstall]" >&2
	exit 2
	;;
esac
