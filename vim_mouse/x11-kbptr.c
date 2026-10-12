/*
 * x11-kbptr: wl-kbptr's tile mode for X11. Covers the screen with a grid of
 * letter labels; typing a label moves the pointer to the centre of that tile
 * (no click). Backspace undoes a letter, Esc cancels. Grid size, label order
 * and colours match wl-kbptr with this directory's wl-kbptr.conf.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <cairo-xlib.h>

/* RGBA. Label colours are wl-kbptr.conf's, backgrounds wl-kbptr's defaults. */
#define LABEL_COLOR 0xebdbb2ffu
#define LABEL_SELECT_COLOR 0xfabd2fffu
#define SELECTABLE_BORDER_COLOR 0xd7992188u
#define SELECTABLE_BG_COLOR 0x00330044u
#define UNSELECTABLE_BG_COLOR 0x22222266u

#define NUM_SYMBOLS 26 /* labels use a-z */
#define MIN_TILE_AREA (25 * 50)
#define MAX_LABEL_LEN 4

static struct {
	Display *dpy;
	Window root, win;
	cairo_surface_t *surface;
	int width, height;

	int rows, columns;
	int tile_w, tile_h;
	int w_off, h_off; /* the first w_off columns / h_off rows are 1 px larger */
	int num_tiles;
	int label_len;

	/* Typed symbols; the first one is the least significant label digit. */
	int input[MAX_LABEL_LEN];
	int next;
} k;

static void layout(void)
{
	int tile_area = k.width * k.height / (NUM_SYMBOLS * NUM_SYMBOLS);
	if (tile_area < MIN_TILE_AREA)
		tile_area = MIN_TILE_AREA;

	k.rows = k.height / (int)sqrt(tile_area / 2.);
	if (k.rows == 0)
		k.rows = 1;
	k.h_off = k.height % k.rows;
	k.tile_h = k.height / k.rows;

	k.columns = k.width / (int)sqrt(tile_area * 2.);
	if (k.columns == 0)
		k.columns = 1;
	k.w_off = k.width % k.columns;
	k.tile_w = k.width / k.columns;

	k.num_tiles = k.rows * k.columns;
	k.label_len = 0;
	for (int n = k.num_tiles; n > 0; n /= NUM_SYMBOLS)
		k.label_len++;
	if (k.label_len > MAX_LABEL_LEN)
		k.label_len = MAX_LABEL_LEN;
}

/* Tiles are numbered column by column. */
static void tile_rect(int idx, int *x, int *y, int *w, int *h)
{
	int column = idx / k.rows, row = idx % k.rows;
	*x = column * k.tile_w + (column < k.w_off ? column : k.w_off);
	*w = k.tile_w + (column < k.w_off);
	*y = row * k.tile_h + (row < k.h_off ? row : k.h_off);
	*h = k.tile_h + (row < k.h_off);
}

static int digit(int idx, int i)
{
	while (i-- > 0)
		idx /= NUM_SYMBOLS;
	return idx % NUM_SYMBOLS;
}

static bool selectable(int idx)
{
	for (int i = 0; i < k.next; i++) {
		if (digit(idx, i) != k.input[i])
			return false;
	}
	return true;
}

static void set_color(cairo_t *cr, uint32_t rgba)
{
	cairo_set_source_rgba(cr, (rgba >> 24) / 255., (rgba >> 16 & 0xff) / 255.,
			      (rgba >> 8 & 0xff) / 255., (rgba & 0xff) / 255.);
}

static void draw(void)
{
	cairo_t *cr = cairo_create(k.surface);
	cairo_push_group(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_color(cr, UNSELECTABLE_BG_COLOR);
	cairo_paint(cr);

	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, fmin(fmax(k.tile_h * 0.5, 8), 100));
	cairo_set_line_width(cr, 1);

	for (int idx = 0; idx < k.num_tiles; idx++) {
		if (!selectable(idx))
			continue;
		int x, y, w, h;
		tile_rect(idx, &x, &y, &w, &h);

		set_color(cr, SELECTABLE_BG_COLOR);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		set_color(cr, SELECTABLE_BORDER_COLOR);
		cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1);
		cairo_stroke(cr);

		/* Typed part highlighted, the rest plain; centred as a whole. */
		char label[MAX_LABEL_LEN + 1];
		for (int i = 0; i < k.label_len; i++)
			label[i] = (char)('a' + digit(idx, i));
		label[k.label_len] = '\0';
		const char *rest = label + k.next;
		char typed[MAX_LABEL_LEN + 1];
		snprintf(typed, sizeof typed, "%.*s", k.next, label);

		cairo_text_extents_t all, te_typed, te_rest;
		cairo_text_extents(cr, label, &all);
		cairo_text_extents(cr, typed, &te_typed);
		cairo_text_extents(cr, rest, &te_rest);
		cairo_move_to(cr, x + (w - te_typed.x_advance - te_rest.x_advance) / 2,
			      y + (int)((h + all.height) / 2));
		set_color(cr, LABEL_SELECT_COLOR);
		cairo_show_text(cr, typed);
		set_color(cr, LABEL_COLOR);
		cairo_show_text(cr, rest);
	}

	/* Replace the window contents in one go (no flicker, no alpha build-up). */
	cairo_pop_group_to_source(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(k.surface);
	XFlush(k.dpy);
}

/* Returns the chosen tile once a full label is typed, -1 otherwise. */
static int type_symbol(int symbol)
{
	if (k.next >= k.label_len)
		return -1;
	int partial = symbol;
	for (int i = k.next - 1; i >= 0; i--)
		partial = partial * NUM_SYMBOLS + k.input[i];
	if (partial >= k.num_tiles) /* no tile has this prefix */
		return -1;
	k.input[k.next++] = symbol;
	return k.next == k.label_len ? partial : -1;
}

/* The launching shortcut's grab lasts until its key is released, so retry. */
static bool grab_keyboard(void)
{
	for (int i = 0; i < 200; i++) {
		if (XGrabKeyboard(k.dpy, k.root, False, GrabModeAsync, GrabModeAsync,
				  CurrentTime) == GrabSuccess)
			return true;
		nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
	}
	return false;
}

static void create_overlay(void)
{
	int screen = DefaultScreen(k.dpy);
	XVisualInfo vi;
	/* Translucency needs a 32-bit visual (and a compositor, which Cinnamon has);
	 * without one the dim background is just opaque. */
	if (!XMatchVisualInfo(k.dpy, screen, 32, TrueColor, &vi)) {
		vi.visual = DefaultVisual(k.dpy, screen);
		vi.depth = DefaultDepth(k.dpy, screen);
	}
	XSetWindowAttributes attrs = {
		.override_redirect = True,
		.colormap = XCreateColormap(k.dpy, k.root, vi.visual, AllocNone),
		.background_pixel = 0,
		.border_pixel = 0,
		.event_mask = ExposureMask,
	};
	k.win = XCreateWindow(k.dpy, k.root, 0, 0, (unsigned)k.width, (unsigned)k.height, 0,
			      vi.depth, InputOutput, vi.visual,
			      CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel |
				      CWEventMask,
			      &attrs);
	XStoreName(k.dpy, k.win, "x11-kbptr");
	k.surface = cairo_xlib_surface_create(k.dpy, k.win, vi.visual, k.width, k.height);
	XMapRaised(k.dpy, k.win);
}


int main(int argc, char **argv)
{
	if (argc > 1) {
		bool help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(help ? stdout : stderr,
			"usage: x11-kbptr\n"
			"  Type a tile's label to move the pointer to its centre (no click).\n"
			"  Backspace undoes a letter, Esc cancels.\n");
		return help ? 0 : 2;
	}

	k.dpy = XOpenDisplay(NULL);
	if (!k.dpy) {
		fprintf(stderr, "x11-kbptr: cannot open X display\n");
		return 1;
	}
	int screen = DefaultScreen(k.dpy);
	k.root = RootWindow(k.dpy, screen);
	k.width = DisplayWidth(k.dpy, screen);
	k.height = DisplayHeight(k.dpy, screen);

	int event, error, major, minor;
	if (!XTestQueryExtension(k.dpy, &event, &error, &major, &minor)) {
		fprintf(stderr, "x11-kbptr: X server lacks the XTEST extension\n");
		return 1;
	}
	if (!grab_keyboard()) {
		fprintf(stderr, "x11-kbptr: cannot grab the keyboard\n");
		return 1;
	}
	layout();
	create_overlay();

	int target = -1;
	bool running = true;
	while (running) {
		XEvent ev;
		XNextEvent(k.dpy, &ev);
		if (ev.type == Expose && ev.xexpose.count == 0) {
			draw();
		} else if (ev.type == KeyPress) {
			KeySym sym = XkbKeycodeToKeysym(k.dpy, (KeyCode)ev.xkey.keycode,
							XkbGroupForCoreState(ev.xkey.state), 0);
			if (sym == XK_Escape) {
				running = false;
			} else if (sym == XK_BackSpace) {
				if (k.next > 0) {
					k.next--;
					draw();
				}
			} else if (sym >= XK_a && sym <= XK_z) {
				target = type_symbol((int)(sym - XK_a));
				if (target >= 0)
					running = false;
				else
					draw();
			}
		}
	}

	/* Overlay gone first, so the window under the target sees a normal enter. */
	cairo_surface_destroy(k.surface);
	XDestroyWindow(k.dpy, k.win);
	XUngrabKeyboard(k.dpy, CurrentTime);
	if (target >= 0) {
		int x, y, w, h;
		tile_rect(target, &x, &y, &w, &h);
		XTestFakeMotionEvent(k.dpy, -1, x + w / 2, y + h / 2, CurrentTime);
	}
	XCloseDisplay(k.dpy);
	return 0;
}
