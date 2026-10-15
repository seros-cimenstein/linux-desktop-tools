/*
 * dark-screenlock X11 backend: pitch-black override-redirect window covering
 * the whole root window, with keyboard and pointer actively grabbed.
 *
 * Unlike ext-session-lock on Wayland, X11 cannot keep the session locked if
 * this process dies: the grabs and the window go away with the connection.
 */
#include <getopt.h>
#include <fcntl.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "core.h"

static Display *dpy;
static Window root, win;
static XIC xic;

static Cursor blank_cursor(void)
{
	static char zero;
	XColor black = { 0 };
	Pixmap pm = XCreateBitmapFromData(dpy, root, &zero, 1, 1);
	Cursor c = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
	XFreePixmap(dpy, pm);
	return c;
}

static void create_window(Cursor cursor)
{
	int scr = DefaultScreen(dpy);
	XSetWindowAttributes a = {
		.override_redirect = True,
		.background_pixel = BlackPixel(dpy, scr),
		.event_mask = KeyPressMask | VisibilityChangeMask,
		.cursor = cursor,
	};
	win = XCreateWindow(dpy, root, 0, 0, (unsigned)DisplayWidth(dpy, scr),
			    (unsigned)DisplayHeight(dpy, scr), 0, CopyFromParent,
			    InputOutput, CopyFromParent,
			    CWOverrideRedirect | CWBackPixel | CWEventMask | CWCursor, &a);
	XStoreName(dpy, win, "dark-screenlock");

	/* Root: StructureNotify for screen resizes (RandR), SubstructureNotify
	 * to re-raise above any window mapped or restacked later. */
	XSelectInput(dpy, root, StructureNotifyMask | SubstructureNotifyMask);
	XMapRaised(dpy, win);
}

/* Something else (an open menu, another locker) may hold a grab briefly. */
static bool grab_input(Cursor cursor)
{
	const struct timespec step = { .tv_nsec = 10 * 1000 * 1000 };
	int kb = GrabNotViewable, ptr = GrabNotViewable;

	for (int i = 0; i < 200 && kb != GrabSuccess; i++) {
		kb = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync,
				   CurrentTime);
		if (kb != GrabSuccess)
			nanosleep(&step, NULL);
	}
	for (int i = 0; i < 200 && ptr != GrabSuccess; i++) {
		ptr = XGrabPointer(dpy, win, False,
				   ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
				   GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);
		if (ptr != GrabSuccess)
			nanosleep(&step, NULL);
	}
	if (kb != GrabSuccess || ptr != GrabSuccess) {
		fprintf(stderr, "dark-screenlock: cannot grab %s (another app holds it)\n",
			kb != GrabSuccess ? "keyboard" : "pointer");
		return false;
	}
	return true;
}

static void setup_input_method(void)
{
	/* Local IM only: UTF-8 text and compose/dead keys, never ibus/fcitx. */
	setlocale(LC_CTYPE, "");
	if (!XSupportsLocale())
		return;
	XSetLocaleModifiers("@im=none");
	XIM im = XOpenIM(dpy, NULL, NULL, NULL);
	if (!im)
		return;
	xic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
			XNClientWindow, win, XNFocusWindow, win, NULL);
}

static size_t latin1_to_utf8(const char *in, int n, char *out)
{
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c < 0x80) {
			out[o++] = (char)c;
		} else {
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	return o;
}

/* Returns true once the password was accepted. */
static bool handle_keypress(XKeyEvent *ev)
{
	char buf[64], raw[16];
	KeySym sym = NoSymbol;
	size_t len = 0;

	if (xic) {
		Status status;
		int n = Xutf8LookupString(xic, ev, buf, sizeof(buf), &sym, &status);
		if (status == XLookupChars || status == XLookupBoth)
			len = (size_t)n;
	} else {
		int n = XLookupString(ev, raw, sizeof(raw), &sym, NULL);
		len = latin1_to_utf8(raw, n, buf);
		explicit_bzero(raw, sizeof(raw));
	}

	KeySym base = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode,
					 XkbGroupForCoreState(ev->state), 0);
	KeySym lower, upper;
	XConvertCase(base, &lower, &upper);

	bool ok = core_key((uint32_t)lower, (uint32_t)sym, ev->state & ControlMask,
			   ev->state & Mod1Mask, buf, len);
	explicit_bzero(buf, sizeof(buf));
	return ok;
}

static void daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		perror("dark-screenlock: fork");
		return;
	}
	if (pid > 0)
		_exit(0); /* Parent: input is grabbed, screen is black. */
	setsid();
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		if (null > STDERR_FILENO)
			close(null);
	}
	core_after_fork();
}

static void usage(FILE *f)
{
	fprintf(f, "Usage: dark-screenlock [-f]\n"
		"\n"
		"Locks the X11 session behind a pitch-black screen.\n"
		"%s"
		"\n"
		"Options:\n"
		"  -f, --daemonize  fork to background once the screen is locked\n"
		"  -h, --help       show this help\n", core_keys_help);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "daemonize", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	bool background = false;
	int c;
	while ((c = getopt_long(argc, argv, "fh", opts, NULL)) != -1) {
		switch (c) {
		case 'f': background = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	if (!core_init())
		return 1;
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "dark-screenlock: cannot open X display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);

	Cursor cursor = blank_cursor();
	create_window(cursor);
	setup_input_method();
	if (!grab_input(cursor))
		return 1;
	XSync(dpy, False);
	if (background)
		daemonize();

	bool unlocked = false;
	while (!unlocked) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (XFilterEvent(&ev, None))
			continue;
		switch (ev.type) {
		case KeyPress:
			unlocked = handle_keypress(&ev.xkey);
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		case MapNotify:
			if (ev.xmap.window != win)
				XRaiseWindow(dpy, win);
			break;
		case ConfigureNotify:
			if (ev.xconfigure.window == root)
				XMoveResizeWindow(dpy, win, 0, 0,
						  (unsigned)ev.xconfigure.width,
						  (unsigned)ev.xconfigure.height);
			else if (ev.xconfigure.window != win)
				XRaiseWindow(dpy, win);
			break;
		}
	}

	core_wipe();
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
