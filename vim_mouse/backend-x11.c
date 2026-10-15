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
#include <time.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
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
	/* Whole-keyboard grab held while any key is down or a menu we saw open
	 * during it is still mapped (see handle_event). */
	bool grabbed;
	/* Mapped popup menu windows (override-redirect, menu window type). */
	Window menus[16];
	int num_menus;
	Atom wm_window_type, menu_types[4];
	/* CLOCK_MONOTONIC ms until which a right click's menu may still appear, 0 if none. */
	int64_t menu_deadline;
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

#define MENU_WAIT_MS 1000 /* how long a right click's menu may take to appear */

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void backend_button(enum vm_button button, bool pressed)
{
	XTestFakeButtonEvent(x.dpy, button == VM_BUTTON_LEFT ? Button1 : Button3, pressed,
			     CurrentTime);
	XFlush(x.dpy);
	/* Keep the keyboard until the menu shows up (see release_keyboard). */
	if (button == VM_BUTTON_RIGHT && !pressed && x.grabbed)
		x.menu_deadline = now_ms() + MENU_WAIT_MS;
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

/* Override-redirect windows typed as menus: what browsers and toolkits pop up. */
static bool is_menu(Window win)
{
	Atom type;
	int format;
	unsigned long count, after;
	unsigned char *data = NULL;
	bool menu = false;
	if (XGetWindowProperty(x.dpy, win, x.wm_window_type, 0, 8, False, XA_ATOM, &type, &format,
			       &count, &after, &data) == Success && data) {
		const Atom *types = (const Atom *)data;
		for (unsigned long i = 0; i < count && !menu; i++) {
			for (size_t t = 0; t < sizeof x.menu_types / sizeof *x.menu_types; t++)
				menu |= types[i] == x.menu_types[t];
		}
	}
	XFree(data);
	return menu;
}

static void forget_menu(Window win)
{
	for (int i = 0; i < x.num_menus; i++) {
		if (x.menus[i] == win) {
			x.menus[i] = x.menus[--x.num_menus];
			return;
		}
	}
}

/*
 * Grabbing the keyboard sends the focused window a FocusOut, and Firefox closes
 * its menus on FocusOut, so a menu opened by Return would close on the next
 * key press, before Space could click an item. The grab Return started is
 * therefore kept until its menu appears (a menu can take a while, Return is
 * usually up by then) and for as long as a menu is open; ungrabbing gives a
 * FocusIn, which closes nothing.
 */
static void release_keyboard(Time time)
{
	if (x.menu_deadline && now_ms() >= x.menu_deadline)
		x.menu_deadline = 0;
	if (x.grabbed && !x.menu_deadline && x.num_menus == 0 && !any_key_down()) {
		XUngrabKeyboard(x.dpy, time);
		x.grabbed = false;
	}
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

		if (!pressed)
			release_keyboard(ev->xkey.time);
	} else if (ev->type == MapNotify) {
		/* Only menus opened while we hold the keyboard: grabbing for one
		 * that is already open would close it. */
		if (x.grabbed && ev->xmap.override_redirect &&
		    x.num_menus < (int)(sizeof x.menus / sizeof *x.menus) && is_menu(ev->xmap.window)) {
			x.menus[x.num_menus++] = ev->xmap.window;
			x.menu_deadline = 0;
		}
	} else if (ev->type == UnmapNotify || ev->type == DestroyNotify) {
		forget_menu(ev->type == UnmapNotify ? ev->xunmap.window : ev->xdestroywindow.window);
		release_keyboard(CurrentTime);
	} else if (ev->type == x.xkb_event) {
		XkbEvent *xkb = (XkbEvent *)ev;
		if (xkb->any.xkb_type == XkbStateNotify)
			set_modifiers(xkb->state.mods);
	}
}

static XErrorHandler default_error_handler;
static int grab_conflicts;

/* BadAccess on XGrabKey: another client already owns that key combination.
 * BadWindow: a window mapped by another client was gone before is_menu(). */
static int on_x_error(Display *dpy, XErrorEvent *err)
{
	if (err->error_code == BadAccess && err->request_code == X_GrabKey) {
		grab_conflicts++;
		return 0;
	}
	if (err->error_code == BadWindow)
		return 0;
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

	default_error_handler = XSetErrorHandler(on_x_error);
	x.wm_window_type = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE", False);
	x.menu_types[0] = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
	x.menu_types[1] = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
	x.menu_types[2] = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE_COMBO", False);
	x.menu_types[3] = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE_MENU", False); /* Chromium */
	XSelectInput(x.dpy, x.root, SubstructureNotifyMask);
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
		int timeout = -1;
		if (x.menu_deadline) {
			int64_t left = x.menu_deadline - now_ms();
			timeout = left > 0 ? (int)left : 0;
		}
		int ready = poll(fds, 1 + VM_NUM_FDS, timeout);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("vim-mouse: poll");
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (ready == 0) /* no menu appeared after the right click */
			release_keyboard(CurrentTime);
		vm_dispatch(fds + 1);
	}

	vm_release_buttons();
	/* Closing the connection drops the key grabs. */
	XDestroyWindow(x.dpy, x.indicator);
	XCloseDisplay(x.dpy);
	return 0;
}
