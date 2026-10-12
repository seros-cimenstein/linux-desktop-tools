#!/bin/sh
# Installed as /usr/local/bin/sway-sched-guard; sway runs it at startup via
# /etc/sway/config.d/90-no-realtime.conf. Switches the sway that started it back to
# SCHED_OTHER however it was launched (/usr/bin/sway directly, a display manager without
# /usr/local/bin in PATH, cap_sys_nice restored). Lowering its own policy needs no root.
pid=${SWAYSOCK%.sock}   # $XDG_RUNTIME_DIR/sway-ipc.<uid>.<pid>.sock
pid=${pid##*.}
exec chrt --other --all-tasks --pid 0 "$pid"
