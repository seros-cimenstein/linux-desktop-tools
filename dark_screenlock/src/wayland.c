/* dark-screenlock Wayland backend: pitch-black ext-session-lock-v1 locker. */
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "core.h"
#include "ext-session-lock-v1-client-protocol.h"

struct output {
	struct wl_list link;
	uint32_t global_name;
	struct wl_output *wl_output;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_buffer *buffer;
	uint32_t width, height;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct ext_session_lock_manager_v1 *lock_manager;
	struct ext_session_lock_v1 *lock;
	struct wl_list outputs;

	struct xkb_context *xkb_ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;

	bool daemonize;
	bool locked;
	bool running;
	int exit_code;
} st;

/* ---- Lock state ---------------------------------------------------------- */

static void unlock(void)
{
	ext_session_lock_v1_unlock_and_destroy(st.lock);
	st.lock = NULL;
	wl_display_roundtrip(st.display);
	st.running = false;
}

/* Keysym of the key without modifiers, in the active layout. */
static xkb_keysym_t base_sym(xkb_keycode_t kc)
{
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(st.xkb_state, kc);
	if (layout == XKB_LAYOUT_INVALID)
		return XKB_KEY_NoSymbol;
	int n = xkb_keymap_key_get_syms_by_level(st.keymap, kc, layout, 0, &syms);
	return n > 0 ? xkb_keysym_to_lower(syms[0]) : XKB_KEY_NoSymbol;
}

static void handle_key(xkb_keycode_t kc)
{
	bool ctrl = xkb_state_mod_name_is_active(st.xkb_state, XKB_MOD_NAME_CTRL,
						 XKB_STATE_MODS_EFFECTIVE) > 0;
	bool alt = xkb_state_mod_name_is_active(st.xkb_state, XKB_MOD_NAME_ALT,
						XKB_STATE_MODS_EFFECTIVE) > 0;

	char buf[64];
	int n = xkb_state_key_get_utf8(st.xkb_state, kc, buf, sizeof(buf));
	size_t len = n > 0 && (size_t)n < sizeof(buf) ? (size_t)n : 0;
	bool ok = core_key(base_sym(kc), xkb_state_key_get_one_sym(st.xkb_state, kc),
			   ctrl, alt, buf, len);
	explicit_bzero(buf, sizeof(buf));
	if (ok && st.locked)
		unlock();
}

/* ---- Keyboard ------------------------------------------------------------ */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
		      int32_t fd, uint32_t size)
{
	(void)data; (void)kb;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
		st.xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap)
		return;
	struct xkb_state *state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}
	xkb_state_unref(st.xkb_state);
	xkb_keymap_unref(st.keymap);
	st.keymap = keymap;
	st.xkb_state = state;
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *surface, struct wl_array *keys)
{
	(void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *surface)
{
	(void)data; (void)kb; (void)serial; (void)surface;
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
		   uint32_t time, uint32_t key, uint32_t state)
{
	(void)data; (void)kb; (void)serial; (void)time;
	if (st.xkb_state && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		handle_key(key + 8);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
			 uint32_t depressed, uint32_t latched, uint32_t locked,
			 uint32_t group)
{
	(void)data; (void)kb; (void)serial;
	if (st.xkb_state)
		xkb_state_update_mask(st.xkb_state, depressed, latched, locked,
				      0, 0, group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
			   int32_t delay)
{
	(void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* ---- Pointer: only used to hide the cursor ------------------------------- */

static void ptr_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
		      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)surface; (void)x; (void)y;
	wl_pointer_set_cursor(ptr, serial, NULL, 0, 0);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{ (void)d; (void)p; (void)s; (void)sf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st_)
{ (void)d; (void)p; (void)s; (void)t; (void)b; (void)st_; }
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void ptr_frame(void *d, struct wl_pointer *p)
{ (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

/* ---- Seat ---------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	(void)data;
	bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;

	if (has_kb && !st.keyboard) {
		st.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(st.keyboard, &keyboard_listener, NULL);
	} else if (!has_kb && st.keyboard) {
		wl_keyboard_release(st.keyboard);
		st.keyboard = NULL;
	}
	if (has_ptr && !st.pointer) {
		st.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(st.pointer, &pointer_listener, NULL);
	} else if (!has_ptr && st.pointer) {
		wl_pointer_release(st.pointer);
		st.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- Outputs / lock surfaces --------------------------------------------- */

static struct wl_buffer *create_black_buffer(uint32_t w, uint32_t h)
{
	/* A fresh memfd reads as zeros, which is black in XRGB8888. */
	size_t stride = (size_t)w * 4, size = stride * h;
	if (size == 0 || size > INT32_MAX)
		return NULL;
	int fd = memfd_create("dark-screenlock", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(
		pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
				   uint32_t serial, uint32_t w, uint32_t h)
{
	struct output *o = data;
	ext_session_lock_surface_v1_ack_configure(ls, serial);

	struct wl_buffer *old = NULL;
	if (!o->buffer || o->width != w || o->height != h) {
		struct wl_buffer *buf = create_black_buffer(w, h);
		if (!buf) {
			fprintf(stderr, "dark-screenlock: cannot allocate %ux%u buffer\n", w, h);
			return;
		}
		old = o->buffer;
		o->buffer = buf;
		o->width = w;
		o->height = h;
	}
	wl_surface_attach(o->surface, o->buffer, 0, 0);
	wl_surface_damage(o->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(o->surface);
	if (old)
		wl_buffer_destroy(old);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

static void output_create_lock_surface(struct output *o)
{
	if (!st.lock || o->surface)
		return;
	o->surface = wl_compositor_create_surface(st.compositor);
	o->lock_surface = ext_session_lock_v1_get_lock_surface(st.lock, o->surface,
							      o->wl_output);
	ext_session_lock_surface_v1_add_listener(o->lock_surface,
						 &lock_surface_listener, o);
}

static void output_destroy(struct output *o)
{
	if (o->lock_surface)
		ext_session_lock_surface_v1_destroy(o->lock_surface);
	if (o->surface)
		wl_surface_destroy(o->surface);
	if (o->buffer)
		wl_buffer_destroy(o->buffer);
	wl_output_destroy(o->wl_output);
	wl_list_remove(&o->link);
	free(o);
}

/* ---- Session lock -------------------------------------------------------- */

static void daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		perror("dark-screenlock: fork");
		return;
	}
	if (pid > 0)
		_exit(0); /* Parent: screen is confirmed locked. */
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

static void lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
	(void)data; (void)lock;
	st.locked = true;
	if (st.daemonize)
		daemonize();
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
	(void)data;
	fprintf(stderr, st.locked
		? "dark-screenlock: compositor ended the lock\n"
		: "dark-screenlock: compositor refused to lock (another locker running?)\n");
	ext_session_lock_v1_destroy(lock);
	st.lock = NULL;
	st.exit_code = 1;
	st.running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

/* ---- Registry ------------------------------------------------------------ */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		st.compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
						 min_u32(version, 4));
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		st.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !st.seat) {
		st.seat = wl_registry_bind(reg, name, &wl_seat_interface,
					   min_u32(version, 5));
		wl_seat_add_listener(st.seat, &seat_listener, NULL);
	} else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
		st.lock_manager = wl_registry_bind(
			reg, name, &ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		if (!o)
			return;
		o->global_name = name;
		o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, 1);
		wl_list_insert(&st.outputs, &o->link);
		output_create_lock_surface(o);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg;
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &st.outputs, link) {
		if (o->global_name == name) {
			output_destroy(o);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- Main ---------------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f, "Usage: dark-screenlock [-f]\n"
		"\n"
		"Locks the Wayland session behind a pitch-black screen.\n"
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
	int c;
	while ((c = getopt_long(argc, argv, "fh", opts, NULL)) != -1) {
		switch (c) {
		case 'f': st.daemonize = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	if (!core_init())
		return 1;
	wl_list_init(&st.outputs);

	st.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	st.display = wl_display_connect(NULL);
	if (!st.xkb_ctx || !st.display) {
		fprintf(stderr, "dark-screenlock: cannot connect to Wayland display\n");
		return 1;
	}

	struct wl_registry *reg = wl_display_get_registry(st.display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(st.display);

	if (!st.compositor || !st.shm || !st.lock_manager) {
		fprintf(stderr, "dark-screenlock: compositor lacks ext-session-lock-v1 support\n");
		return 1;
	}

	st.lock = ext_session_lock_manager_v1_lock(st.lock_manager);
	ext_session_lock_v1_add_listener(st.lock, &lock_listener, NULL);
	struct output *o;
	wl_list_for_each(o, &st.outputs, link)
		output_create_lock_surface(o);

	st.running = true;
	while (st.running && wl_display_dispatch(st.display) != -1)
		;
	if (st.running) {
		/* Session stays locked by the compositor; rerun to recover. */
		fprintf(stderr, "dark-screenlock: lost connection to compositor\n");
		st.exit_code = 1;
	}

	core_wipe();
	wl_display_disconnect(st.display);
	return st.exit_code;
}
