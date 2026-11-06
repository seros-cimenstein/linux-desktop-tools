# linux-desktop-tools

Small Linux desktop fixes and utilities I wrote for my own machines (a ThinkPad T480 running
sway on CachyOS, and a second laptop running Cinnamon on X11). Each folder is a separate tool
with its own README and installer; they do not depend on each other.

| Tool | What it does | Language |
|---|---|---|
| [vim_mouse](vim_mouse/) | Keyboard-driven mouse for sway and X11: label grid to jump the pointer, vim keys to glide and click | C |
| [dark_screenlock](dark_screenlock/) | Pitch-black screen locker for Wayland (ext-session-lock-v1) and X11 with blind PAM password entry | C |
| [touch-gap-bridge](touch-gap-bridge/) | Bridges short contact dropouts and freezes when an active stylus is used on a finger-only touchscreen | Python |
| [quietmode](quietmode/) | Caps CPU frequency (and Intel RAPL package power) so the laptop fans stay off; switch one or several hosts over ssh | Bash |
| [stutter_fix](stutter_fix/) | Fix for ~1 s sway input freezes (repeated letters) caused by sway running realtime, with the diagnosis and measurement tools | Bash, Python |
| [usb-luks](usb-luks/) | Offers LUKS2 encryption for USB drives when they are plugged in, keeping their data, and auto-unlocks them afterwards | Bash |

## Status

These are personal tools, tested only on the machines described in each README. Several of
them install system files, sudoers rules, udev rules or systemd units, or run as root; read
the installer before running it.

- `dark_screenlock` and `usb-luks` deal with security but have **not been security-audited**.
- `usb-luks` **re-encrypts drives**. Back up a drive before letting it encrypt it.

## License

MIT, see [LICENSE](LICENSE).
