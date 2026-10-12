/*
 * vim-mouse: smooth keyboard pointer control for wlroots compositors.
 *
 * Grabs the keyboard through a tiny layer-shell indicator and moves the
 * pointer through wlr-virtual-pointer while h/j/k/l (or arrows) are held,
 * with warpd-style acceleration. Space holds the left button for as long as
 * it is held (click or drag). Runs until Esc; launching it again while it
 * runs stops the running instance (toggle from one keybinding).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define TICK_NS 8000000L /* 125 Hz motion updates */
#define INDICATOR_SIZE 12
#define INDICATOR_MARGIN 6
#define INDICATOR_ARGB 0xffb8bb26u /* gruvbox green */
#define MAX_KEYCODE 768

enum dir { LEFT, DOWN, UP, RIGHT, NUM_DIRS };

static struct {
	double speed;      /* px/s when a direction key goes down */
	double max_speed;  /* px/s cap */
	double accel;      /* px/s^2 */
	double fast_accel; /* px/s^2 while Ctrl is held */
	double slow_speed; /* fixed px/s while Shift is held */
} opt = {220, 1600, 700, 2900, 50};

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

	/* Direction bound to each currently held keycode, -1 if none. */
	signed char key_dir[MAX_KEYCODE];
	int held[NUM_DIRS];
	bool fast, slow;
	/* Keyboard focus; gliding pauses without it (sway bounces it on clicks). */
	bool focused;

	/* Keycode holding the left button down, -1 if the button is up. */
	int drag_key;

	int timer_fd;
	bool moving;
	double velocity;
	struct timespec last_tick;
	/* Speed when gliding was paused by focus loss (0 if not paused). */
	double paused_velocity;
	struct timespec paused_at;

	bool running;
	int signal_fd;
} st;

static double elapsed_s(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void pointer_motion(double dx, double dy)
{
	zwlr_virtual_pointer_v1_motion(st.pointer, now_ms(),
				       wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(st.pointer);
	wl_display_flush(st.display);
}

static void pointer_button(uint32_t button, enum wl_pointer_button_state state)
{
	zwlr_virtual_pointer_v1_button(st.pointer, now_ms(), button, state);
	zwlr_virtual_pointer_v1_frame(st.pointer);
	wl_display_flush(st.display);
}

static void pointer_click(uint32_t button)
{
	pointer_button(button, WL_POINTER_BUTTON_STATE_PRESSED);
	pointer_button(button, WL_POINTER_BUTTON_STATE_RELEASED);
}

/* Never leave the left button stuck down (focus loss, exit, toggle-off). */
static void release_drag(void)
{
	if (st.drag_key >= 0) {
		pointer_button(BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		st.drag_key = -1;
	}
}

static void set_timer(bool on)
{
	struct itimerspec spec = {0};
	if (on) {
		spec.it_interval.tv_nsec = TICK_NS;
		spec.it_value.tv_nsec = TICK_NS;
	}
	timerfd_settime(st.timer_fd, 0, &spec, NULL);
}

static void direction(double *x, double *y)
{
	*x = (st.held[RIGHT] > 0) - (st.held[LEFT] > 0);
	*y = (st.held[DOWN] > 0) - (st.held[UP] > 0);
}

/* Start or stop the motion loop to match the held direction keys. */
static void update_motion(void)
{
	double x, y;
	direction(&x, &y);
	bool want = st.focused && (x != 0 || y != 0);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (want && !st.moving) {
		/* Resuming right after sway's click focus bounce keeps the speed. */
		bool bounce = st.paused_velocity > 0 && elapsed_s(&st.paused_at, &now) < 0.1;
		st.velocity = bounce ? st.paused_velocity : st.slow ? opt.slow_speed : opt.speed;
		st.paused_velocity = 0;
		st.last_tick = now;
		if (!bounce) {
			/* First step right away so a quick tap still nudges the pointer. */
			double norm = hypot(x, y);
			pointer_motion(st.velocity * TICK_NS / 1e9 * x / norm,
				       st.velocity * TICK_NS / 1e9 * y / norm);
		}
		set_timer(true);
	} else if (!want && st.moving) {
		set_timer(false);
		if (!st.focused) {
			st.paused_velocity = st.velocity;
			st.paused_at = now;
		}
	}
	st.moving = want;
}

static void tick(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double dt = elapsed_s(&st.last_tick, &now);
	st.last_tick = now;
	if (dt > 0.05) /* stalled (e.g. suspend): don't jump */
		dt = 0.05;

	double x, y;
	direction(&x, &y);
	if (x == 0 && y == 0)
		return;

	if (st.slow) {
		st.velocity = opt.slow_speed;
	} else {
		st.velocity += (st.fast ? opt.fast_accel : opt.accel) * dt;
		if (st.velocity > opt.max_speed)
			st.velocity = opt.max_speed;
	}

	double norm = hypot(x, y);
	pointer_motion(st.velocity * dt * x / norm, st.velocity * dt * y / norm);
}

/* ---- keyboard ---- */

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
			    int32_t fd, uint32_t size)
{
	xkb_state_unref(st.xkb_state);
	xkb_keymap_unref(st.xkb_keymap);
	st.xkb_state = NULL;
	st.xkb_keymap = NULL;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	st.xkb_keymap = xkb_keymap_new_from_string(st.xkb_context, map,
						   XKB_KEYMAP_FORMAT_TEXT_V1,
						   XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (st.xkb_keymap)
		st.xkb_state = xkb_state_new(st.xkb_keymap);
}

static bool key_in(const struct wl_array *keys, uint32_t key)
{
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
	for (uint32_t key = 0; key < MAX_KEYCODE; key++) {
		if (st.key_dir[key] >= 0 && !key_in(keys, key)) {
			st.held[(int)st.key_dir[key]]--;
			st.key_dir[key] = -1;
		}
	}
	if (st.drag_key >= 0 && !key_in(keys, (uint32_t)st.drag_key))
		release_drag();

	/* Keys already down that we never saw pressed (e.g. the launching
	 * shortcut) stay ignored: their release finds nothing in key_dir. */
	st.focused = true;
	update_motion();
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
			   struct wl_surface *surface)
{
	/* Pause gliding (we can't see releases now); state is reconciled on enter. */
	st.focused = false;
	update_motion();
}

/* Keysym ignoring modifiers, so Shift+h / Ctrl+h still read as h. */
static xkb_keysym_t base_keysym(xkb_keycode_t code)
{
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(st.xkb_state, code);
	if (xkb_keymap_key_get_syms_by_level(st.xkb_keymap, code, layout, 0, &syms) < 1)
		return XKB_KEY_NoSymbol;
	return syms[0];
}

static int keysym_dir(xkb_keysym_t sym)
{
	switch (sym) {
	case XKB_KEY_h: case XKB_KEY_Left:  return LEFT;
	case XKB_KEY_j: case XKB_KEY_Down:  return DOWN;
	case XKB_KEY_k: case XKB_KEY_Up:    return UP;
	case XKB_KEY_l: case XKB_KEY_Right: return RIGHT;
	default: return -1;
	}
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
			 uint32_t time, uint32_t key, uint32_t state)
{
	if (key >= MAX_KEYCODE)
		return;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (st.key_dir[key] >= 0) {
			st.held[(int)st.key_dir[key]]--;
			st.key_dir[key] = -1;
			update_motion();
		} else if ((int)key == st.drag_key) {
			release_drag();
		}
		return;
	}

	if (!st.xkb_state)
		return;
	xkb_keysym_t sym = base_keysym(key + 8);

	int dir = keysym_dir(sym);
	if (dir >= 0) {
		if (st.key_dir[key] < 0) {
			st.key_dir[key] = (signed char)dir;
			st.held[dir]++;
			update_motion();
		}
		return;
	}

	switch (sym) {
	case XKB_KEY_space:
		if (st.drag_key < 0) {
			pointer_button(BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			st.drag_key = (int)key;
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		pointer_click(BTN_RIGHT);
		break;
	case XKB_KEY_Escape:
		st.running = false;
		break;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
			       uint32_t depressed, uint32_t latched, uint32_t locked,
			       uint32_t group)
{
	if (!st.xkb_state)
		return;
	xkb_state_update_mask(st.xkb_state, depressed, latched, locked, 0, 0, group);
	st.slow = xkb_state_mod_name_is_active(st.xkb_state, XKB_MOD_NAME_SHIFT,
					       XKB_STATE_MODS_EFFECTIVE) > 0;
	st.fast = xkb_state_mod_name_is_active(st.xkb_state, XKB_MOD_NAME_CTRL,
					       XKB_STATE_MODS_EFFECTIVE) > 0;
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
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st.keyboard) {
		st.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(st.keyboard, &keyboard_listener, NULL);
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
		st.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		st.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !st.seat) {
		st.seat = wl_registry_bind(registry, name, &wl_seat_interface,
					   version < 5 ? version : 5);
		wl_seat_add_listener(st.seat, &seat_listener, NULL);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		st.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		st.pointer_manager = wl_registry_bind(registry, name,
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
	if (!st.configured) {
		wl_surface_attach(st.surface, st.buffer, 0, 0);
		wl_surface_commit(st.surface);
		st.configured = true;
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	st.running = false;
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
		px[i] = INDICATOR_ARGB;
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, INDICATOR_SIZE, INDICATOR_SIZE,
							  stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void create_indicator(void)
{
	st.buffer = indicator_buffer();
	st.surface = wl_compositor_create_surface(st.compositor);

	/* Empty input region: the pointer passes straight through the square. */
	struct wl_region *region = wl_compositor_create_region(st.compositor);
	wl_surface_set_input_region(st.surface, region);
	wl_region_destroy(region);

	st.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		st.layer_shell, st.surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "vim-mouse");
	zwlr_layer_surface_v1_add_listener(st.layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(st.layer_surface, INDICATOR_SIZE, INDICATOR_SIZE);
	zwlr_layer_surface_v1_set_anchor(st.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
							   ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_margin(st.layer_surface, INDICATOR_MARGIN, INDICATOR_MARGIN, 0, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(st.layer_surface, 1); /* exclusive */
	wl_surface_commit(st.surface);
}

/* ---- single instance toggle ---- */

/* Returns true if another instance was running (and has been told to stop). */
static bool stop_running_instance(void)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[512];
	snprintf(path, sizeof path, "%s/vim-mouse.pid", dir ? dir : "/tmp");

	/* Kept open (and locked) for the lifetime of this process. */
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror("vim-mouse: lock file");
		exit(1);
	}
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		char pid[32];
		int n = snprintf(pid, sizeof pid, "%d\n", getpid());
		if (ftruncate(fd, 0) < 0 || pwrite(fd, pid, (size_t)n, 0) != n)
			perror("vim-mouse: writing pid");
		return false;
	}
	if (errno != EWOULDBLOCK) {
		perror("vim-mouse: flock");
		exit(1);
	}
	char buf[32] = {0};
	if (pread(fd, buf, sizeof buf - 1, 0) > 0) {
		pid_t pid = (pid_t)strtol(buf, NULL, 10);
		if (pid > 0)
			kill(pid, SIGTERM);
	}
	return true;
}

/* ---- main ---- */

static void usage(FILE *out)
{
	fprintf(out,
		"usage: vim-mouse [options]\n"
		"  hjkl/arrows move (hold; diagonals work), Shift = slow, Ctrl = faster,\n"
		"  Space = hold left button (click/drag), Return = right click, Esc = quit.\n"
		"  Running it again while active stops the active instance.\n\n"
		"  --speed N       initial speed px/s      (default %g)\n"
		"  --max-speed N   maximum speed px/s      (default %g)\n"
		"  --accel N       acceleration px/s^2     (default %g)\n"
		"  --fast-accel N  acceleration with Ctrl  (default %g)\n"
		"  --slow-speed N  speed with Shift px/s   (default %g)\n",
		opt.speed, opt.max_speed, opt.accel, opt.fast_accel, opt.slow_speed);
}

static void parse_args(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"speed", required_argument, NULL, 's'},
		{"max-speed", required_argument, NULL, 'm'},
		{"accel", required_argument, NULL, 'a'},
		{"fast-accel", required_argument, NULL, 'f'},
		{"slow-speed", required_argument, NULL, 'w'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		double *target = NULL;
		switch (c) {
		case 's': target = &opt.speed; break;
		case 'm': target = &opt.max_speed; break;
		case 'a': target = &opt.accel; break;
		case 'f': target = &opt.fast_accel; break;
		case 'w': target = &opt.slow_speed; break;
		case 'h': usage(stdout); exit(0);
		default: usage(stderr); exit(2);
		}
		char *end;
		*target = strtod(optarg, &end);
		if (*end || *target <= 0) {
			fprintf(stderr, "vim-mouse: invalid value '%s'\n", optarg);
			exit(2);
		}
	}
}

int main(int argc, char **argv)
{
	parse_args(argc, argv);
	if (stop_running_instance())
		return 0;

	memset(st.key_dir, -1, sizeof st.key_dir);
	st.drag_key = -1;
	st.running = true;

	st.display = wl_display_connect(NULL);
	if (!st.display) {
		fprintf(stderr, "vim-mouse: cannot connect to wayland display\n");
		return 1;
	}
	st.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	struct wl_registry *registry = wl_display_get_registry(st.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(st.display);

	if (!st.compositor || !st.shm || !st.seat || !st.layer_shell || !st.pointer_manager) {
		fprintf(stderr, "vim-mouse: compositor lacks wl_compositor, wl_shm, wl_seat, "
				"wlr-layer-shell or wlr-virtual-pointer\n");
		return 1;
	}

	st.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(st.pointer_manager,
									     st.seat);
	/* sway hides the cursor after touch input; pointer activity unhides it. */
	pointer_motion(0, 0);

	st.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (st.timer_fd < 0) {
		perror("vim-mouse: timerfd");
		return 1;
	}

	create_indicator();

	/* SIGTERM (toggle-off) / SIGINT / SIGHUP end the loop so cleanup runs. */
	sigset_t quit_signals;
	sigemptyset(&quit_signals);
	sigaddset(&quit_signals, SIGTERM);
	sigaddset(&quit_signals, SIGINT);
	sigaddset(&quit_signals, SIGHUP);
	sigprocmask(SIG_BLOCK, &quit_signals, NULL);
	st.signal_fd = signalfd(-1, &quit_signals, SFD_CLOEXEC);
	if (st.signal_fd < 0) {
		perror("vim-mouse: signalfd");
		return 1;
	}

	struct pollfd fds[3] = {
		{.fd = wl_display_get_fd(st.display), .events = POLLIN},
		{.fd = st.timer_fd, .events = POLLIN},
		{.fd = st.signal_fd, .events = POLLIN},
	};
	while (st.running) {
		while (wl_display_prepare_read(st.display) != 0)
			wl_display_dispatch_pending(st.display);
		wl_display_flush(st.display);

		if (poll(fds, 3, -1) < 0) {
			wl_display_cancel_read(st.display);
			if (errno == EINTR)
				continue;
			perror("vim-mouse: poll");
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(st.display) < 0)
				break;
		} else {
			wl_display_cancel_read(st.display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(st.display) < 0)
			break;

		if (fds[1].revents & POLLIN) {
			uint64_t expirations;
			if (read(st.timer_fd, &expirations, sizeof expirations) > 0)
				tick();
		}
		if (fds[2].revents & POLLIN)
			st.running = false;
	}

	release_drag();
	zwlr_layer_surface_v1_destroy(st.layer_surface);
	wl_surface_destroy(st.surface);
	wl_buffer_destroy(st.buffer);
	zwlr_virtual_pointer_v1_destroy(st.pointer);
	wl_display_roundtrip(st.display);
	wl_display_disconnect(st.display);
	return 0;
}
