#!/usr/bin/env bash
# ./install.sh [uninstall]   — run from your normal account; re-execs via sudo.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
((EUID == 0)) || exec sudo "$PWD/install.sh" "$@"
user=${SUDO_USER:?run ./install.sh from your normal account, not a root shell}

case ${1:-install} in
install)
	conf=conf/$(hostname).conf
	[[ -f $conf ]] || { echo "no $conf; copy one of conf/*.conf and adjust it" >&2; exit 1; }
	install -Dm755 quietmode /usr/local/bin/quietmode
	install -Dm644 "$conf" /etc/quietmode.conf
	install -Dm644 quietmode.service /etc/systemd/system/quietmode.service

	# Lets `quietmode on|off` (locally and over ssh) run without a password.
	tmp=$(mktemp)
	printf '%s ALL=(root) NOPASSWD: /usr/local/bin/quietmode on, /usr/local/bin/quietmode off\n' "$user" >"$tmp"
	visudo -cqf "$tmp"
	install -m440 "$tmp" /etc/sudoers.d/quietmode
	rm -f "$tmp"

	systemctl daemon-reload
	systemctl enable quietmode.service
	# Pick up config changes if quiet mode is already on.
	[[ -f /var/lib/quietmode/enabled ]] && /usr/local/bin/quietmode on
	echo "installed: quietmode on | off | status"
	;;
uninstall)
	[[ -x /usr/local/bin/quietmode ]] && /usr/local/bin/quietmode off || true
	systemctl disable quietmode.service 2>/dev/null || true
	rm -f /usr/local/bin/quietmode /etc/quietmode.conf /etc/systemd/system/quietmode.service /etc/sudoers.d/quietmode
	rm -rf /var/lib/quietmode /run/quietmode
	systemctl daemon-reload
	echo "uninstalled"
	;;
*)
	echo "usage: $0 [install|uninstall]" >&2
	exit 2
	;;
esac
