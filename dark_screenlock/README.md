# dark_screenlock

A screen locker that shows nothing: every output goes pitch black, the cursor is hidden, and
there is no prompt or password field. Every key is ignored until the arm combo is pressed;
only then is a typed password collected (blind) and checked with PAM.

| Key | Action |
|---|---|
| `Ctrl+Alt+P` | enable password entry (all other keys are ignored until then); pressed again while armed, it discards what was typed |
| `Enter` | submit the password; a wrong one disables entry again, so the combo is needed again |
| `Alt+C` | discard what was typed, stay armed |
| `Backspace` | delete the last character |

Two backends, chosen at build time:

- **wayland**: an `ext-session-lock-v1` client (sway, Hyprland and other compositors that
  implement the protocol). If the locker loses its connection, the session stays locked by the
  compositor; run it again to get back in.
- **x11**: an override-redirect black window over the whole root window with keyboard and
  pointer grabbed. X11 cannot keep the session locked if this process dies: the grabs and the
  window go away with it.

The password buffer is `mlock`ed (kept out of swap) and wiped after each attempt. PAM
authenticates against the `login` service (`LOCK_PAM_SERVICE` in `src/core.c`).

## Build and install

```
make                    # Wayland backend (default)
make BACKEND=x11        # X11 backend
make install            # into ~/.local/bin (PREFIX / BINDIR / DESTDIR are honoured)
make uninstall
```

Build deps: a C compiler, `pkg-config`, PAM headers, and for Wayland `wayland-client`,
`xkbcommon`, `wayland-protocols` (for `ext-session-lock-v1.xml`) and `wayland-scanner`; for X11
`libX11`.

## Usage

```
dark-screenlock         # lock; returns once unlocked
dark-screenlock -f      # fork to the background once the screen is locked
```

For example in sway: `bindsym $mod+Escape exec dark-screenlock`.

## Status

Personal tool. It has **not been security-audited**; do not
rely on it where a locker's correctness matters. On X11 in particular, a crash of the locker
unlocks the session.
