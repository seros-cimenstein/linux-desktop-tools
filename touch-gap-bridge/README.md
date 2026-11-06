# touch-gap-bridge

Bridges short contact dropouts on a finger-only touchscreen that is used with an active stylus.
On such a panel the pen's contact is lost for a few frames now and then, so a stroke splits
into several (a "break"), or the line freezes and then jumps (a "hitch").

`touch-gap-bridge` grabs the touchscreen exclusively and re-emits it through a uinput clone
named `<original name> (gap bridge)`:

- **Gap bridging**: every lift is held back for `--gap` ms. If a new contact lands within
  `--dist` device units of where the held one lifted, it continues the same contact (same
  tracking ID) instead of starting a new stroke. Otherwise the held lift is released when it
  expires.
- **Smoothing**: output is played back about `--smooth` ms behind the pen, at a rate steered by
  how much data is buffered. When the panel stops reporting, playback slows down instead of
  freezing, and catches up when data resumes. Between reported points the path follows a
  Hermite curve that keeps the pen's direction into and out of each gap.

## Requirements

- Linux with `uinput`, Python 3 and [python-evdev](https://pypi.org/project/evdev/).
- Read access to the touchscreen's `/dev/input/event*` and write access to `/dev/uinput`
  (in practice: root, as the systemd unit runs it).

## Usage

```
sudo ./touch-gap-bridge                              # defaults: --match Raydium --gap 200 --dist 60 --smooth 50
sudo ./touch-gap-bridge --match "<panel name>" --verbose
```

| Option | Meaning |
|---|---|
| `--match` | substring of the touchscreen's input device name (default `Raydium`) |
| `--gap` | longest dropout to bridge, ms (default 200) |
| `--dist` | longest jump across a dropout, device units (default 60) |
| `--smooth` | target playback lag used to smooth over freezes, ms; 0 = off (default 50) |
| `--verbose` | print every bridged/split dropout and stroke freeze |

To run it at boot:

```
sudo install -Dm755 touch-gap-bridge /usr/local/bin/touch-gap-bridge
sudo install -Dm644 touch-gap-bridge.service /etc/systemd/system/touch-gap-bridge.service
sudo systemctl enable --now touch-gap-bridge.service
```

The unit restarts the bridge when the device disappears (suspend, unplug).

## pen-test.html

A standalone page to check the result: open it in a browser and draw. It counts and marks
breaks (the line split; red dashed link) and hitches (the line froze, then jumped; amber).
`C` clears, `D` shows sample dots, `F` toggles fullscreen.

When the page is served over http it also posts every sample to `/log` on the same server;
that logging server is not part of this repo, so open the file directly (`file://`).

## Status

Written for one laptop's Raydium touchscreen; the defaults are tuned for that panel. Other
panels need their own `--match`, and probably different `--gap`/`--dist` values.
