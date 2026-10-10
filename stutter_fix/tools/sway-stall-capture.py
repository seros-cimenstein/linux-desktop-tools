#!/usr/bin/env python3
"""Sample sway's main thread every 1 ms; log busy (R/D) stretches > 120 ms with IPC latency,
user/kernel CPU split, GPU IRQ deltas, kernel uevents and processes started just before."""
import os, socket, struct, sys, threading, time

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 180
CLK = os.sysconf("SC_CLK_TCK")
BOOT = time.time() - float(open("/proc/uptime").read().split()[0])
SWAY = next(int(p) for p in os.listdir("/proc") if p.isdigit() and open(f"/proc/{p}/comm").read().strip() == "sway")


def stat(p):
    """state, utime, stime, starttime (ticks)"""
    try:
        s = open(f"/proc/{p}/stat").read()
        r = s[s.rfind(")") + 2:].split()
        return r[0], int(r[11]), int(r[12]), int(r[19])
    except OSError:
        return None, 0, 0, 0


def gpu_irqs():
    out = {}
    for line in open("/proc/interrupts"):
        parts = line.split()
        name = parts[-1] if parts else ""
        if name in ("i915", "nvidia"):
            out[name] = out.get(name, 0) + sum(int(x) for x in parts[1:] if x.isdigit())
    return out


def recent_procs(since_wall):
    out = []
    for p in os.listdir("/proc"):
        if p.isdigit() and BOOT + stat(p)[3] / CLK >= since_wall:
            try:
                out.append(open(f"/proc/{p}/comm").read().strip())
            except OSError:
                pass
    return sorted(set(out))

def all_cpu():
    """{tid: (stime, utime, comm)} for every thread on the system."""
    out = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            tids = os.listdir(f"/proc/{p}/task")
        except OSError:
            continue
        for t in tids:
            try:
                s = open(f"/proc/{p}/task/{t}/stat").read()
            except OSError:
                continue
            r = s[s.rfind(")") + 2:].split()
            out[int(t)] = (int(r[12]), int(r[11]), s[s.find("(") + 1:s.rfind(")")])
    return out
snaps = []


def snapshotter():
    while True:
        snaps.append((time.monotonic(), all_cpu()))
        del snaps[:-8]
        time.sleep(0.25)


uevents = []


def uevent_listener():
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 15)  # NETLINK_KOBJECT_UEVENT, kernel group
    s.bind((0, 1))
    while True:
        uevents.append((time.monotonic(), s.recv(65536).split(b"\0")[0].decode(errors="replace")))


ipc_log = []


def ipc():
    run = f"/run/user/{os.getuid()}"
    sock_path = os.environ.get("SWAYSOCK") or next(f"{run}/{f}" for f in os.listdir(run) if f.startswith("sway-ipc"))
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    req = b"i3-ipc" + struct.pack("=II", 0, 7)
    while True:
        t0 = time.monotonic()
        s.sendall(req)
        h = b""
        while len(h) < 14:
            h += s.recv(14 - len(h))
        n = struct.unpack("=6sII", h)[1]
        b = b""
        while len(b) < n:
            b += s.recv(n - len(b))
        ipc_log.append((t0, (time.monotonic() - t0) * 1000))
        time.sleep(0.02)


threading.Thread(target=uevent_listener, daemon=True).start()
threading.Thread(target=ipc, daemon=True).start()
threading.Thread(target=snapshotter, daemon=True).start()
_, u0, s0, _ = stat(SWAY)
episodes, busy_since = 0, None
end = time.monotonic() + DURATION
while time.monotonic() < end:
    now = time.monotonic()
    st, ut, kt, _ = stat(SWAY)
    if st in ("R", "D"):
        if busy_since is None:
            busy_since, states, wall, ut_a, kt_a, irq_a = now, set(), time.time(), ut, kt, gpu_irqs()
        states.add(st)
    elif busy_since is not None:
        ms = (now - busy_since) * 1000
        if ms > 120:
            episodes += 1
            irq_b = gpu_irqs()
            ipc_max = max([x[1] for x in ipc_log if busy_since - 0.05 <= x[0] <= now] or [0])
            ue = [u for t, u in uevents if busy_since - 3 <= t <= now]
            print(f"{time.strftime('%H:%M:%S', time.localtime(wall))} busy {ms:.0f}ms state={''.join(sorted(states))} "
                  f"user={(ut - ut_a) * 1000 // CLK}ms kernel={(kt - kt_a) * 1000 // CLK}ms ipc_max={ipc_max:.0f}ms "
                  f"irq={ {k: irq_b[k] - irq_a.get(k, 0) for k in irq_b} } uevents(-3s)={ue[:6]} "
                  f"new={recent_procs(wall - 3)[:12]}", flush=True)
            snap_a = next((s for t, s in reversed(snaps) if t < busy_since), {})
            snap_b = all_cpu()
            top = sorted(((snap_b[k][0] - snap_a[k][0], snap_b[k][1] - snap_a[k][1], k) for k in snap_b if k in snap_a),
                         reverse=True)[:6]
            print("    others (kernel ms, user ms):",
                  [(f"{snap_b[k][2]}[{k}]", ks * 1000 // CLK, us * 1000 // CLK) for ks, us, k in top if ks or us], flush=True)
        busy_since = None
    time.sleep(0.001)

_, u1, s1, _ = stat(SWAY)
lat = sorted(x[1] for x in ipc_log)
print(f"done: {DURATION:.0f}s episodes={episodes} sway user={(u1 - u0) / CLK:.1f}s kernel={(s1 - s0) / CLK:.1f}s "
      f"uevents={len(uevents)} ipc p50={lat[len(lat) // 2]:.1f} p99={lat[int(len(lat) * .99)]:.1f} max={lat[-1]:.0f}ms",
      flush=True)
