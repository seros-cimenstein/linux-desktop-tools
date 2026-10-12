#!/bin/sh
# Run sway with normal (non-realtime) scheduling, now and after every sway upgrade.
set -eu
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
here=$(cd "$(dirname "$0")" && pwd)

# Future sessions: drop the capability sway uses to make its main thread SCHED_RR.
setcap -r /usr/bin/sway 2>/dev/null || true
# The sway package re-adds it in post_upgrade; this hook removes it again afterwards.
install -Dm644 "$here/sway-no-realtime.hook" /etc/pacman.d/hooks/sway-no-realtime.hook
# Without the capability, RLIMIT_RTPRIO (@audio rtprio 99) still lets sway go SCHED_RR.
# This wrapper starts sway with a soft rtprio limit of 0.
install -Dm755 "$here/sway-wrapper.sh" /usr/local/bin/sway
# Running session: switch every sway thread to SCHED_OTHER (no logout needed).
for pid in $(pidof sway || true); do
    chrt --other --all-tasks --pid 0 "$pid"
done

echo "sway capabilities: $(getcap /usr/bin/sway || true)"
echo "sway wrapper: $(command -v sway) -> $(tail -n1 /usr/local/bin/sway)"
for pid in $(pidof sway || true); do
    chrt --pid "$pid"
done
