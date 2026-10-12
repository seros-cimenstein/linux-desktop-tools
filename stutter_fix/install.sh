#!/usr/bin/env bash
# ./install.sh [uninstall]   — run sway with normal (non-realtime) scheduling, now and for good.
# Re-runs itself with sudo.
set -euo pipefail
[[ $(id -u) -eq 0 ]] || exec sudo "$0" "$@"
cd "$(dirname "$(readlink -f "$0")")"

HOOK=/etc/pacman.d/hooks/sway-no-realtime.hook
WRAPPER=/usr/local/bin/sway
GUARD=/usr/local/bin/sway-sched-guard
GUARD_CONF=/etc/sway/config.d/90-no-realtime.conf

case ${1:-install} in
install)
	# Drop the capability sway uses to make its main thread SCHED_RR...
	setcap -r /usr/bin/sway 2>/dev/null || true
	# ...and remove it again after every sway upgrade (post_upgrade re-adds it).
	install -Dm644 sway-no-realtime.hook "$HOOK"
	# Without the capability, RLIMIT_RTPRIO (@audio rtprio 99) still allows SCHED_RR:
	# start sway with a soft rtprio limit of 0.
	install -Dm755 sway-wrapper.sh "$WRAPPER"
	# Last line of defence for launches that bypass the wrapper: sway resets itself at startup.
	install -Dm755 sway-sched-guard.sh "$GUARD"
	install -Dm644 90-no-realtime.conf "$GUARD_CONF"
	# Running session: switch every sway thread to SCHED_OTHER (no logout needed).
	for pid in $(pidof sway || true); do
		chrt --other --all-tasks --pid 0 "$pid"
	done

	echo "sway capabilities: $(getcap /usr/bin/sway || true)"
	echo "sway in PATH: $(command -v sway)"
	for pid in $(pidof sway || true); do
		chrt --pid "$pid" | head -n1
	done
	echo "installed"
	;;
uninstall)
	rm -f "$HOOK" "$WRAPPER" "$GUARD" "$GUARD_CONF"
	setcap cap_sys_nice+ep /usr/bin/sway   # as shipped by the sway package
	echo "uninstalled: takes effect at the next sway start"
	;;
*)
	echo "usage: $0 [install|uninstall]" >&2
	exit 2
	;;
esac
