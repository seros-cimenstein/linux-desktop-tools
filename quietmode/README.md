# quietmode

Caps CPU frequency (and on Intel, package power) so the laptop runs cool enough that the fans stay off under sustained load.

```
quietmode on            # this machine
quietmode on laptop       # another machine over ssh
quietmode on all        # this machine + PEERS from /etc/quietmode.conf
quietmode off [...]     # restore previous limits
quietmode status [...]  # caps, temperature, fan RPM
```

## What `on` does

- PPD profile → `power-saver` (previous profile saved in `/var/lib/quietmode/ppd-orig`)
- Turbo/boost off (`intel_pstate/no_turbo`, or `cpufreq/boost` where it exists)
- `scaling_max_freq` on every policy → `MAX_FREQ_MHZ`
- Intel RAPL package limits PL1/PL2 → `RAPL_LONG_W`/`RAPL_SHORT_W` (MSR and MMIO interfaces)

Original sysfs values are saved to `/run/quietmode/orig` and restored by `off`. The mode survives reboot and suspend: `quietmode.service` reapplies it while `/var/lib/quietmode/enabled` exists.

## Install

Per-host settings are in `conf/<hostname>.conf`.

```
./install.sh            # this machine (asks for sudo password once)
./deploy.sh laptop        # copies the repo to laptop:~/quietmode and installs there
./install.sh uninstall  # or: ./deploy.sh laptop uninstall
```

The installer adds `/etc/sudoers.d/quietmode` so your user can run `quietmode on`/`off` without a password, which also makes `quietmode on laptop` work over non-interactive ssh.

To tune, edit `conf/<hostname>.conf` and re-run the install. If the fan still spins under full load, lower `MAX_FREQ_MHZ` (or `RAPL_LONG_W` on the T480).
