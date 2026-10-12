#!/usr/bin/env python3
"""Root-only: catch sway's kernel stack while it spins inside a syscall.

When sway's main thread has been runnable for > 300 ms, trigger SysRq-L (backtrace of all
active CPUs; read-only, goes to the kernel log) once per freeze. Stops after 3 freezes or
180 s, then prints the backtraces of the CPU that was running sway.

    sudo python3 tools/sway-freeze-stack.py
"""
import os, subprocess, time

SWAY = next(int(p) for p in os.listdir("/proc") if p.isdigit() and open(f"/proc/{p}/comm").read().strip() == "sway")


def state() -> str:
    s = open(f"/proc/{SWAY}/stat").read()
    return s[s.rfind(")") + 2]


start = int(time.time()) - 1
print(f"watching sway pid {SWAY}; keep using the desktop until 3 freezes are caught (max 180 s)", flush=True)
caught, busy_since, triggered = 0, None, False
end = time.monotonic() + 180
while caught < 3 and time.monotonic() < end:
    now = time.monotonic()
    if state() in "RD":
        busy_since = busy_since or now
        if not triggered and now - busy_since > 0.3:
            with open("/proc/sysrq-trigger", "w") as f:
                f.write("l")
            triggered = True
    elif busy_since is not None:
        ms = (now - busy_since) * 1000
        if ms > 300:
            caught += 1
            print(f"freeze {caught}: {ms:.0f} ms", flush=True)
        busy_since, triggered = None, False
    time.sleep(0.002)

if not caught:
    print("no freezes > 300 ms caught")
    raise SystemExit(0)

# The dmesg ring buffer may be full and wrapping, so select by time via the journal.
kernel_log = subprocess.run(["journalctl", "-k", "-o", "cat", "--no-pager", "--since", f"@{start}"],
                            capture_output=True, text=True).stdout.splitlines()
block, keep = [], False
for line in kernel_log + ["NMI backtrace for cpu <end>"]:
    if "NMI backtrace for cpu" in line or "sysrq: Show backtrace" in line:
        if keep:
            print("\n".join(block), "\n", flush=True)
        block, keep = [line], False
        continue
    block.append(line)
    if f" PID: {SWAY} " in line:
        keep = True
