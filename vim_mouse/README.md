# vim_mouse

Keyboard-driven mouse on sway: jump the pointer anywhere with `$mod+g` (wl-kbptr), then glide and click with vim keys with `$mod+c` (vim-mouse, in this directory).

| Key | Action |
|---|---|
| `$mod+g` | label grid over the screen; type the 2 letters to move the pointer to that tile (no click) |
| `$mod+c` | start vim-mouse (green square top-right); press again or `Esc` to stop |
| hold `h j k l` / arrows | glide, accelerating like warpd (220 px/s, +700 px/s², max 1600); two keys = diagonal |
| `Shift` + direction | slow, steady 50 px/s |
| `Ctrl` + direction | accelerate ~4× faster |
| hold `Space` | left button stays down while held: tap = click, hold + glide = drag |
| `Return` | right click |

Speeds are flags on the binding, e.g. `exec vim-mouse --slow-speed 30`; see `vim-mouse --help`.

## Install

```
./install.sh            # builds vim-mouse + patched wl-kbptr into ~/.local, links wl-kbptr.conf
./install.sh uninstall
```

Then add the two bindings it prints to `~/.config/sway/config` and `swaymsg reload`. Build deps: `wayland`, `wayland-protocols`, `libxkbcommon`, `cairo`, `meson`. No root needed.

## Why it's built this way

- **vim-mouse vs a sway mode**: sway bindings fire on key repeat, so `seat - cursor move` jumps in steps after a 600 ms repeat delay. vim-mouse grabs the keyboard through a 12 px layer-shell square and drives a wlr-virtual-pointer at 125 Hz from key press/release, so motion is continuous and sub-pixel.
- **sway hides the cursor after touchscreen input** and then sends no pointer focus, so IPC moves/clicks go nowhere. vim-mouse sends a zero-length virtual-pointer motion on start, which counts as device activity and unhides it.
- **Clicking a window bounces keyboard focus** off an exclusive layer surface (`seat_set_focus` in sway's `seat.c`: leave + enter in the same millisecond). vim-mouse therefore only pauses on `leave`, and on `enter` releases whatever isn't in the compositor's list of still-pressed keys, so holds survive clicks and the button can't get stuck.
- **wl-kbptr patch** (`wl-kbptr-null-keymap.patch`): upstream v0.4.1 segfaults when sway sends modifiers before a keymap (happens after a virtual keyboard such as `wtype` was the last input).
