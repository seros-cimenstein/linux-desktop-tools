# vim_mouse

Keyboard-driven mouse on sway and on X11 desktops such as Cinnamon: jump the pointer anywhere with `$mod+g` (wl-kbptr on sway, x11-kbptr on X11), then glide and click with vim keys with `$mod+c` (vim-mouse). Everything here lives in this directory except wl-kbptr.

| Key | Action |
|---|---|
| `$mod+g` | label grid over the screen; type the 2 letters to move the pointer to that tile (no click); `Backspace` undoes a letter, `Esc` cancels |
| `$mod+c` | start vim-mouse (green square top-right); press again or `Esc` to stop |
| hold `h j k l` / arrows | glide, accelerating like warpd (220 px/s, +700 px/s², max 1600); two keys = diagonal |
| `Shift` + direction | slow, steady 50 px/s |
| `Ctrl` + direction | accelerate ~4× faster |
| `Space` | tap = left click; hold (300 ms) or glide while it's down = left button held for a drag |
| `Return` | right click |

Speeds are flags on the binding, e.g. `exec vim-mouse --slow-speed 30`; see `vim-mouse --help`.

## Install

```
./install.sh            # into ~/.local, no root
./install.sh uninstall
```

The backend follows the session (`$WAYLAND_DISPLAY`, else `$DISPLAY`); over ssh set `VIM_MOUSE_BACKEND=wayland|x11` (and `DISPLAY` / `DBUS_SESSION_BUS_ADDRESS` so the Cinnamon bindings land in the running session).

- **sway**: builds vim-mouse and patched wl-kbptr, links `wl-kbptr.conf`, then prints the two bindings to add to `~/.config/sway/config` before `swaymsg reload`. Build deps: `wayland`, `wayland-protocols`, `libxkbcommon`, `cairo`, `meson`.
- **X11**: builds vim-mouse and x11-kbptr. On Cinnamon it adds `Super+g` / `Super+c` as custom shortcuts (uninstall removes them); other window managers get the commands printed. Build deps: X11, Xtst, Xext and cairo headers. If they're missing on Debian/Ubuntu/Mint, install.sh fetches the `-dev` packages with `apt-get download` into `~/.local/src/vim-mouse-x11-dev` and links against the runtime libraries already installed, so no root is needed there either.

`make BACKEND=wayland|x11` builds into `build/<backend>/`.

## Why it's built this way

- **vim-mouse vs a sway mode**: sway bindings fire on key repeat, so `seat - cursor move` jumps in steps after a 600 ms repeat delay. vim-mouse grabs the keyboard through a 12 px layer-shell square and drives a wlr-virtual-pointer at 125 Hz from key press/release, so motion is continuous and sub-pixel.
- **sway hides the cursor after touchscreen input** and then sends no pointer focus, so IPC moves/clicks go nowhere. vim-mouse sends a zero-length virtual-pointer motion on start, which counts as device activity and unhides it.
- **Clicking a window bounces keyboard focus** off an exclusive layer surface (`seat_set_focus` in sway's `seat.c`: leave + enter in the same millisecond). vim-mouse therefore only pauses on `leave`, and on `enter` releases whatever isn't in the compositor's list of still-pressed keys, so holds survive clicks and the button can't get stuck.
- **A Space tap releases the keyboard for the click**: when sway takes the focus back from the clicked window it unfocuses it, which closes that window's popups (`send_unfocus` → `view_close_popups`) before the click arrives, so a context-menu item (Return, then Space on "Open link in new tab") could never be clicked. vim-mouse waits for Space's release to tell a tap from a hold, then sets the layer surface's keyboard interactivity to none, sends press + release, and grabs again. Drags keep the grab (the release must be seen), so the button goes down only after 300 ms of holding or when a glide starts.
- **wl-kbptr patch** (`wl-kbptr-null-keymap.patch`): upstream v0.4.1 segfaults when sway sends modifiers before a keymap (happens after a virtual keyboard such as `wtype` was the last input).
- **X11: only the bound keys are grabbed** (without Super/Alt), not the whole keyboard: a client holding the keyboard starves the window manager's shortcuts, so `Super+c` couldn't stop vim-mouse and `Super+g` couldn't open the grid on top of it. Unbound keys therefore still reach the focused window while vim-mouse runs (sway swallows them). Once a bound key goes down the whole keyboard is held until every key is up, so staggered releases of diagonals and drags aren't lost.
- **X11: the grab outlives Return while its menu is open**: grabbing the keyboard sends the focused window a FocusOut, and Firefox closes its menus on FocusOut, so the first glide key after Return closed the menu. After a right click vim-mouse keeps the keyboard for up to 1 s waiting for a menu (override-redirect window typed popup/dropdown menu, combo or Chromium's menu), then until every such menu is unmapped; ungrabbing only sends a FocusIn. Unbound keys pressed in that time are swallowed, and Esc quits vim-mouse rather than closing the menu.
- **X11 motion is absolute** (XTest): relative XTest motion goes through the server's pointer acceleration and would distort the glide.
- **x11-kbptr** reimplements wl-kbptr's tile mode (same grid size, label order, `wl-kbptr.conf` colours built in), since wl-kbptr is Wayland-only.

`vim-mouse.c` holds the glide, key state and option handling; `backend-wayland.c` / `backend-x11.c` hold the keyboard grab, indicator and pointer injection.
