/*
 * vim-mouse Wayland backend (wlroots compositors): grabs the keyboard through
 * a tiny layer-shell indicator and moves the pointer through
 * wlr-virtual-pointer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "vim-mouse.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
	struct zwlr_virtual_pointer_v1 *pointer;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_buffer *buffer;
	bool configured;

	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
} wl;

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void backend_motion(double dx, double dy)
{
	zwlr_virtual_pointer_v1_motion(wl.pointer, now_ms(),
				       wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(wl.pointer);
	wl_display_flush(wl.display);
}

void backend_button(enum vm_button button, bool pressed)
{
	zwlr_virtual_pointer_v1_button(wl.pointer, now_ms(),
				       button == VM_BUTTON_LEFT ? BTN_LEFT : BTN_RIGHT,
				       pressed ? WL_POINTER_BUTTON_STATE_PRESSED
					       : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(wl.pointer);
	wl_display_flush(wl.display);
}

/* ---- keyboard ---- */

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
			    int32_t fd, uint32_t size)
{
	xkb_state_unref(wl.xkb_state);
	xkb_keymap_unref(wl.xkb_keymap);
	wl.xkb_state = NULL;
	wl.xkb_keymap = NULL;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	wl.xkb_keymap = xkb_keymap_new_from_string(wl.xkb_context, map,
						   XKB_KEYMAP_FORMAT_TEXT_V1,
						   XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (wl.xkb_keymap)
		wl.xkb_state = xkb_state_new(wl.xkb_keymap);
}

static bool key_in(uint32_t key, void *data)
{
	const struct wl_array *keys = data;
	const uint32_t *k;
	wl_array_for_each(k, keys) {
		if (*k == key)
			return true;
	}
	return false;
}

/*
 * sway bounces keyboard focus off an exclusive layer surface on every click
 * into a window (leave + enter within the same millisecond), so leave must not
 * drop held keys or the left button. Instead, enter's list of currently
 * pressed keys is authoritative: anything we think is held but isn't there
 * was released while we lacked focus.
 */
static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
			   struct wl_surface *surface, struct wl_array *keys)
{
	vm_sync_keys(key_in, keys);
	vm_focus(true);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
			   struct wl_surface *surface)
{
	/* Pause gliding (we can't see releases now); state is reconciled on enter. */
	vm_focus(false);
}

static enum vm_action keysym_action(xkb_keysym_t sym)
{
#define K(name, action) case XKB_KEY_##name: return action;
	switch (sym) {
	VM_BINDINGS(K)
	default: return VM_NONE;
	}
#undef K
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
			 uint32_t time, uint32_t key, uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		vm_key(key, VM_NONE, false);
		return;
	}
	if (!wl.xkb_state)
		return;

	/* Level 0 keysym: modifiers don't change the binding. */
	xkb_keycode_t code = key + 8;
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(wl.xkb_state, code);
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	if (xkb_keymap_key_get_syms_by_level(wl.xkb_keymap, code, layout, 0, &syms) > 0)
		sym = syms[0];
	vm_key(key, keysym_action(sym), true);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
			       uint32_t depressed, uint32_t latched, uint32_t locked,
			       uint32_t group)
{
	if (!wl.xkb_state)
		return;
	xkb_state_update_mask(wl.xkb_state, depressed, latched, locked, 0, 0, group);
	vm_modifiers(xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_SHIFT,
						  XKB_STATE_MODS_EFFECTIVE) > 0,
		     xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_CTRL,
						  XKB_STATE_MODS_EFFECTIVE) > 0);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
				 int32_t delay)
{
	/* Motion is driven by press/release, not key repeat. */
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.keyboard) {
		wl.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(wl.keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- globals / surface ---- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		wl.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !wl.seat) {
		wl.seat = wl_registry_bind(registry, name, &wl_seat_interface,
					   version < 5 ? version : 5);
		wl_seat_add_listener(wl.seat, &seat_listener, NULL);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		wl.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		wl.pointer_manager = wl_registry_bind(registry, name,
						      &zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			    uint32_t serial, uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	if (!wl.configured) {
		wl_surface_attach(wl.surface, wl.buffer, 0, 0);
		wl_surface_commit(wl.surface);
		wl.configured = true;
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	vm_quit();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static struct wl_buffer *indicator_buffer(void)
{
	const int stride = INDICATOR_SIZE * 4;
	const size_t size = (size_t)stride * INDICATOR_SIZE;

	int fd = memfd_create("vim-mouse", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		perror("vim-mouse: memfd");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		perror("vim-mouse: mmap");
		exit(1);
	}
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xff000000u | INDICATOR_RGB;
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(wl.shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, INDICATOR_SIZE, INDICATOR_SIZE,
							  stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void create_indicator(void)
{
	wl.buffer = indicator_buffer();
	wl.surface = wl_compositor_create_surface(wl.compositor);

	/* Empty input region: the pointer passes straight through the square. */
	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	wl_surface_set_input_region(wl.surface, region);
	wl_region_destroy(region);

	wl.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		wl.layer_shell, wl.surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "vim-mouse");
	zwlr_layer_surface_v1_add_listener(wl.layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(wl.layer_surface, INDICATOR_SIZE, INDICATOR_SIZE);
	zwlr_layer_surface_v1_set_anchor(wl.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
							   ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_margin(wl.layer_surface, INDICATOR_MARGIN, INDICATOR_MARGIN, 0, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(wl.layer_surface, 1); /* exclusive */
	wl_surface_commit(wl.surface);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
	if (!vm_init(argc, argv))
		return 0;

	wl.display = wl_display_connect(NULL);
	if (!wl.display) {
		fprintf(stderr, "vim-mouse: cannot connect to wayland display\n");
		return 1;
	}
	wl.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	struct wl_registry *registry = wl_display_get_registry(wl.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(wl.display);

	if (!wl.compositor || !wl.shm || !wl.seat || !wl.layer_shell || !wl.pointer_manager) {
		fprintf(stderr, "vim-mouse: compositor lacks wl_compositor, wl_shm, wl_seat, "
				"wlr-layer-shell or wlr-virtual-pointer\n");
		return 1;
	}

	wl.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(wl.pointer_manager,
									     wl.seat);
	/* sway hides the cursor after touch input; pointer activity unhides it. */
	backend_motion(0, 0);

	create_indicator();

	struct pollfd fds[1 + VM_NUM_FDS] = {
		{.fd = wl_display_get_fd(wl.display), .events = POLLIN},
	};
	vm_fds(fds + 1);
	while (vm_running()) {
		while (wl_display_prepare_read(wl.display) != 0)
			wl_display_dispatch_pending(wl.display);
		wl_display_flush(wl.display);

		if (poll(fds, 1 + VM_NUM_FDS, -1) < 0) {
			wl_display_cancel_read(wl.display);
			if (errno == EINTR)
				continue;
			perror("vim-mouse: poll");
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(wl.display) < 0)
				break;
		} else {
			wl_display_cancel_read(wl.display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(wl.display) < 0)
			break;

		vm_dispatch(fds + 1);
	}

	vm_release_buttons();
	zwlr_layer_surface_v1_destroy(wl.layer_surface);
	wl_surface_destroy(wl.surface);
	wl_buffer_destroy(wl.buffer);
	zwlr_virtual_pointer_v1_destroy(wl.pointer);
	wl_display_roundtrip(wl.display);
	wl_display_disconnect(wl.display);
	return 0;
}
