/*
 * vim-mouse: smooth keyboard pointer control.
 *
 * Moves the pointer while h/j/k/l (or arrows) are held, with warpd-style
 * acceleration. Space holds the left button for as long as it is held
 * (click or drag). Runs until Esc; launching it again while it runs stops
 * the running instance (toggle from one keybinding). The keyboard grab,
 * indicator and pointer injection live in backend-wayland.c / backend-x11.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "vim-mouse.h"

#define TICK_NS 8000000L /* 125 Hz motion updates */
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
	/* Keycodes we saw pressed and not yet released. */
	bool down[MAX_KEYCODE];
	/* Direction bound to each currently held keycode, -1 if none. */
	signed char key_dir[MAX_KEYCODE];
	int held[NUM_DIRS];
	bool fast, slow;
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

void vm_release_buttons(void)
{
	if (st.drag_key >= 0) {
		backend_button(VM_BUTTON_LEFT, false);
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
			backend_motion(st.velocity * TICK_NS / 1e9 * x / norm,
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
	backend_motion(st.velocity * dt * x / norm, st.velocity * dt * y / norm);
}

/* ---- keys ---- */

void vm_key(uint32_t code, enum vm_action action, bool pressed)
{
	if (code >= MAX_KEYCODE)
		return;

	if (!pressed) {
		/* Keys already down before we saw them (e.g. the launching
		 * shortcut) stay ignored: their release finds nothing here. */
		if (!st.down[code])
			return;
		st.down[code] = false;
		if (st.key_dir[code] >= 0) {
			st.held[(int)st.key_dir[code]]--;
			st.key_dir[code] = -1;
			update_motion();
		} else if ((int)code == st.drag_key) {
			vm_release_buttons();
		}
		return;
	}

	if (st.down[code]) /* key repeat */
		return;
	st.down[code] = true;

	switch (action) {
	case VM_LEFT: case VM_DOWN: case VM_UP: case VM_RIGHT: {
		static const enum dir dirs[] = {
			[VM_LEFT] = LEFT, [VM_DOWN] = DOWN, [VM_UP] = UP, [VM_RIGHT] = RIGHT,
		};
		enum dir dir = dirs[action];
		st.key_dir[code] = (signed char)dir;
		st.held[dir]++;
		update_motion();
		break;
	}
	case VM_DRAG:
		if (st.drag_key < 0) {
			backend_button(VM_BUTTON_LEFT, true);
			st.drag_key = (int)code;
		}
		break;
	case VM_MENU:
		backend_button(VM_BUTTON_RIGHT, true);
		backend_button(VM_BUTTON_RIGHT, false);
		break;
	case VM_QUIT:
		st.running = false;
		break;
	case VM_NONE:
		break;
	}
}

void vm_modifiers(bool slow, bool fast)
{
	st.slow = slow;
	st.fast = fast;
}

void vm_focus(bool focused)
{
	st.focused = focused;
	update_motion();
}

void vm_sync_keys(bool (*still_down)(uint32_t code, void *data), void *data)
{
	for (uint32_t code = 0; code < MAX_KEYCODE; code++) {
		if (st.down[code] && !still_down(code, data))
			vm_key(code, VM_NONE, false);
	}
}

/* ---- loop plumbing ---- */

void vm_fds(struct pollfd fds[VM_NUM_FDS])
{
	fds[0] = (struct pollfd){.fd = st.timer_fd, .events = POLLIN};
	fds[1] = (struct pollfd){.fd = st.signal_fd, .events = POLLIN};
}

void vm_dispatch(const struct pollfd fds[VM_NUM_FDS])
{
	if (fds[0].revents & POLLIN) {
		uint64_t expirations;
		if (read(st.timer_fd, &expirations, sizeof expirations) > 0)
			tick();
	}
	if (fds[1].revents & POLLIN)
		st.running = false;
}

bool vm_running(void)
{
	return st.running;
}

void vm_quit(void)
{
	st.running = false;
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

/* ---- options / init ---- */

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

bool vm_init(int argc, char **argv)
{
	parse_args(argc, argv);
	if (stop_running_instance())
		return false;

	memset(st.key_dir, -1, sizeof st.key_dir);
	st.drag_key = -1;
	st.running = true;

	st.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (st.timer_fd < 0) {
		perror("vim-mouse: timerfd");
		exit(1);
	}

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
		exit(1);
	}
	return true;
}
