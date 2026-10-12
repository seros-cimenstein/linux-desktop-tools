#!/bin/sh
# Installed as /usr/local/bin/sway (ahead of /usr/bin in PATH).
# sway makes its main thread SCHED_RR whenever the kernel lets it (sway/realtime.c). Without
# cap_sys_nice it is still allowed through RLIMIT_RTPRIO: cachyos-settings gives @audio
# "rtprio 99" in /etc/security/limits.d/20-audio.conf. Drop only the soft limit to 0 so that
# call fails; the hard limit stays at 99, so programs started from sway can raise it again.
exec prlimit --rtprio=0: /usr/bin/sway "$@"
