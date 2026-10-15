/*
 * vim-mouse X11 backend (e.g. Cinnamon): grabs the bound keys, injects
 * pointer motion and buttons with XTest, and shows the indicator as a small
 * override-redirect window.
 *
 * Unlike the Wayland backend it does not grab the whole keyboard: an X client
 * holding it starves the window manager's shortcuts, so Super+c could not
 * toggle vim-mouse off and Super+g could not open the label grid. Only the
 * bound keys are grabbed, and only without Super/Alt; other keys still reach
 * the focused window.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include "vim-mouse.h"

static struct {
	Display *dpy;
	Window root, indicator;
	int width, height;
	int xkb_event;
	/* Whole-keyboard grab held while any key is down (see handle_event). */
	bool grabbed;
	/* Sub-pixel pointer position and the pixel we last moved it to. */
	double x, y;
	int sent_x, sent_y;
} x;

/*
 * Motion is absolute: XTest relative motion goes through the server's
 * pointer acceleration, which would distort the glide.
 */
void backend_motion(double dx, double dy)
{
	Window root, child;
	int px, py, wx, wy;
	unsigned int mask;
	if (!XQueryPointer(x.dpy, x.root, &root, &child, &px, &py, &wx, &wy, &mask))
		return; /* pointer is on another X screen */

	/* The real mouse moved (or the server clamped us): continue from there. */
	if (px != x.sent_x || py != x.sent_y) {
		x.x = px;
		x.y = py;
	}
	x.x = fmin(fmax(x.x + dx, 0), x.width - 1);
	x.y = fmin(fmax(x.y + dy, 0), x.height - 1);
	x.sent_x = (int)lround(x.x);
	x.sent_y = (int)lround(x.y);
	if (x.sent_x != px || x.sent_y != py) {
		XTestFakeMotionEvent(x.dpy, -1, x.sent_x, x.sent_y, CurrentTime);
		XFlush(x.dpy);
	}
}

void backend_button(enum vm_button button, bool pressed)
{
	XTestFakeButtonEvent(x.dpy, button == VM_BUTTON_LEFT ? Button1 : Button3, pressed,
			     CurrentTime);
	XFlush(x.dpy);
}

void backend_click(void)
{
	backend_button(VM_BUTTON_LEFT, true);
	backend_button(VM_BUTTON_LEFT, false);
}

static enum vm_action keysym_action(KeySym sym)
{
#define K(name, action) case XK_##name: return action;
	switch (sym) {
	VM_BINDINGS(K)
	default: return VM_NONE;
	}
#undef K
}

static void set_modifiers(unsigned int mods)
{
	vm_modifiers(mods & ShiftMask, mods & ControlMask);
}

static bool any_key_down(void)
{
	char keys[32];
	XQueryKeymap(x.dpy, keys);
	for (size_t i = 0; i < sizeof keys; i++) {
		if (keys[i])
			return true;
	}
	return false;
}

static void handle_event(XEvent *ev)
{
	if (ev->type == KeyPress || ev->type == KeyRelease) {
		bool pressed = ev->type == KeyPress;
		/* A passive key grab ends when that key is released, so the release of
		 * a key pressed meanwhile (diagonals, Space during a glide) would go to
		 * the focused window. Keep the keyboard until every key is up. */
		if (pressed && !x.grabbed)
			x.grabbed = XGrabKeyboard(x.dpy, x.root, False, GrabModeAsync, GrabModeAsync,
						  ev->xkey.time) == GrabSuccess;

		/* Level 0 keysym: modifiers don't change the binding. */
		KeySym sym = XkbKeycodeToKeysym(x.dpy, (KeyCode)ev->xkey.keycode,
						XkbGroupForCoreState(ev->xkey.state), 0);
		vm_key(ev->xkey.keycode, keysym_action(sym), pressed);

		if (!pressed && x.grabbed && !any_key_down()) {
			XUngrabKeyboard(x.dpy, ev->xkey.time);
			x.grabbed = false;
		}
	} else if (ev->type == x.xkb_event) {
		XkbEvent *xkb = (XkbEvent *)ev;
		if (xkb->any.xkb_type == XkbStateNotify)
			set_modifiers(xkb->state.mods);
	}
}

static XErrorHandler default_error_handler;
static int grab_conflicts;

/* BadAccess on XGrabKey: another client already owns that key combination. */
static int on_x_error(Display *dpy, XErrorEvent *err)
{
	if (err->error_code == BadAccess && err->request_code == X_GrabKey) {
		grab_conflicts++;
		return 0;
	}
	return default_error_handler(dpy, err);
}

static void grab_keys(void)
{
	static const KeySym keys[] = {
#define K(name, action) XK_##name,
		VM_BINDINGS(K)
#undef K
	};
	/* Shift / Ctrl change the speed; Caps Lock and Num Lock must not matter. */
	static const unsigned int mods[] = {0, ShiftMask, ControlMask, ShiftMask | ControlMask};
	static const unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

	default_error_handler = XSetErrorHandler(on_x_error);
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		KeyCode code = XKeysymToKeycode(x.dpy, keys[i]);
		if (!code)
			continue;
		for (size_t m = 0; m < sizeof mods / sizeof *mods; m++) {
			for (size_t l = 0; l < sizeof locks / sizeof *locks; l++)
				XGrabKey(x.dpy, code, mods[m] | locks[l], x.root, False,
					 GrabModeAsync, GrabModeAsync);
		}
	}
	XSync(x.dpy, False);
	XSetErrorHandler(default_error_handler);
	if (grab_conflicts)
		fprintf(stderr, "vim-mouse: %d key combinations are taken by other shortcuts\n",
			grab_conflicts);
}

static void create_indicator(void)
{
	int screen = DefaultScreen(x.dpy);
	XColor color = {
		.red = (INDICATOR_RGB >> 16 & 0xff) * 0x101,
		.green = (INDICATOR_RGB >> 8 & 0xff) * 0x101,
		.blue = (INDICATOR_RGB & 0xff) * 0x101,
		.flags = DoRed | DoGreen | DoBlue,
	};
	XAllocColor(x.dpy, DefaultColormap(x.dpy, screen), &color);

	XSetWindowAttributes attrs = {.override_redirect = True, .background_pixel = color.pixel};
	x.indicator = XCreateWindow(x.dpy, x.root,
				    x.width - INDICATOR_SIZE - INDICATOR_MARGIN, INDICATOR_MARGIN,
				    INDICATOR_SIZE, INDICATOR_SIZE, 0, CopyFromParent, InputOutput,
				    CopyFromParent, CWOverrideRedirect | CWBackPixel, &attrs);
	/* Empty input shape: the pointer passes straight through the square. */
	XShapeCombineRectangles(x.dpy, x.indicator, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	XMapRaised(x.dpy, x.indicator);
}

int main(int argc, char **argv)
{
	if (!vm_init(argc, argv))
		return 0;

	x.dpy = XOpenDisplay(NULL);
	if (!x.dpy) {
		fprintf(stderr, "vim-mouse: cannot open X display\n");
		return 1;
	}
	int screen = DefaultScreen(x.dpy);
	x.root = RootWindow(x.dpy, screen);
	x.width = DisplayWidth(x.dpy, screen);
	x.height = DisplayHeight(x.dpy, screen);
	x.sent_x = x.sent_y = -1;

	int opcode, error, major = XkbMajorVersion, minor = XkbMinorVersion;
	int xtest_event, xtest_error;
	if (!XkbQueryExtension(x.dpy, &opcode, &x.xkb_event, &error, &major, &minor) ||
	    !XTestQueryExtension(x.dpy, &xtest_event, &xtest_error, &major, &minor)) {
		fprintf(stderr, "vim-mouse: X server lacks the XKB or XTEST extension\n");
		return 1;
	}
	/* Repeats arrive as extra presses (ignored) instead of release+press. */
	XkbSetDetectableAutoRepeat(x.dpy, True, NULL);
	XkbSelectEventDetails(x.dpy, XkbUseCoreKbd, XkbStateNotify, XkbModifierStateMask,
			      XkbModifierStateMask);
	XkbStateRec state;
	if (XkbGetState(x.dpy, XkbUseCoreKbd, &state) == Success)
		set_modifiers(state.mods);

	grab_keys();
	create_indicator();
	vm_focus(true);

	struct pollfd fds[1 + VM_NUM_FDS] = {
		{.fd = ConnectionNumber(x.dpy), .events = POLLIN},
	};
	vm_fds(fds + 1);
	while (vm_running()) {
		/* Also drains events queued while waiting for XQueryPointer replies. */
		while (vm_running() && XPending(x.dpy)) {
			XEvent ev;
			XNextEvent(x.dpy, &ev);
			handle_event(&ev);
		}
		if (!vm_running())
			break;
		if (poll(fds, 1 + VM_NUM_FDS, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("vim-mouse: poll");
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		vm_dispatch(fds + 1);
	}

	vm_release_buttons();
	/* Closing the connection drops the key grabs. */
	XDestroyWindow(x.dpy, x.indicator);
	XCloseDisplay(x.dpy);
	return 0;
}
