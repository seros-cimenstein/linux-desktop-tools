/*
 * vim-mouse core: glide/acceleration, key state, options and the
 * single-instance toggle, shared by the Wayland and X11 backends.
 */
#ifndef VIM_MOUSE_H
#define VIM_MOUSE_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#define INDICATOR_SIZE 12
#define INDICATOR_MARGIN 6
#define INDICATOR_RGB 0xb8bb26u /* gruvbox green */

enum vm_action { VM_NONE = -1, VM_LEFT, VM_DOWN, VM_UP, VM_RIGHT, VM_DRAG, VM_MENU, VM_QUIT };
enum vm_button { VM_BUTTON_LEFT, VM_BUTTON_RIGHT };

/*
 * Key bindings by keysym name. Backends expand K into switch cases over
 * XKB_KEY_<name> (Wayland) or XK_<name> (X11); the two share keysym values.
 * Lookups ignore modifiers, so Shift+h / Ctrl+h still read as h.
 */
#define VM_BINDINGS(K)                                  \
	K(h, VM_LEFT) K(Left, VM_LEFT)                  \
	K(j, VM_DOWN) K(Down, VM_DOWN)                  \
	K(k, VM_UP) K(Up, VM_UP)                        \
	K(l, VM_RIGHT) K(Right, VM_RIGHT)               \
	K(space, VM_DRAG)                               \
	K(Return, VM_MENU) K(KP_Enter, VM_MENU)         \
	K(Escape, VM_QUIT)

/* Implemented by the backend. */
void backend_motion(double dx, double dy);
void backend_button(enum vm_button button, bool pressed);

/* Parses options and takes the single-instance lock. Returns false if it
 * stopped an already running instance instead (the caller should exit 0). */
bool vm_init(int argc, char **argv);

/* The core's timer and signal fds, for the backend's poll loop. */
#define VM_NUM_FDS 2
void vm_fds(struct pollfd fds[VM_NUM_FDS]);
void vm_dispatch(const struct pollfd fds[VM_NUM_FDS]);
bool vm_running(void);
void vm_quit(void);

/* Key press/release by backend keycode; action only matters on press.
 * Repeated presses of a key that is already down are ignored. */
void vm_key(uint32_t code, enum vm_action action, bool pressed);
void vm_modifiers(bool slow, bool fast);
/* Gliding pauses without keyboard focus. */
void vm_focus(bool focused);
/* Release every key we consider down for which still_down(code) is false. */
void vm_sync_keys(bool (*still_down)(uint32_t code, void *data), void *data);
/* Never leave the left button stuck down: call before exiting. */
void vm_release_buttons(void);

#endif
